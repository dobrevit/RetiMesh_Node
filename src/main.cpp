// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

// ============================================================================
//  RetiMesh Node — standalone Reticulum LoRa gateway for ESP32-S3
//
//  main.cpp owns the pieces the modules share — the two ring buffers and
//  the FreeRTOS task layout — and wires everything together in setup().
//
//  ┌────────────────────────── CORE 0 ──────────────────────────┐
//  │  Wi-Fi / LwIP stack (ESP-IDF system tasks)                 │
//  │  AsyncTCP event task    socket I/O for ports 80 and 4242   │
//  │  displayTask            OLED status page, 2 Hz             │
//  └────────────────────────────────────────────────────────────┘
//  ┌────────────────────────── CORE 1 ──────────────────────────┐
//  │  radioTask   (prio 5)   SX1262 IRQs, CSMA, fragmentation   │
//  │  rns task    (prio 3)   Reticulum Transport + interfaces   │
//  │  loopTask    (prio 1)   heartbeat log                      │
//  └────────────────────────────────────────────────────────────┘
//
//  Packet flow end to end:
//
//   Sideband/RNS client        this node                     LoRa channel
//   ─────────────────────────────────────────────────────────────────────
//   TCP :4242 ──HDLC──► AsyncTCP task ──► tcpInRing ─┐
//                                                     ├─► Transport (rns task)
//   RF ──► radioTask ──► rxRing ─────────────────────┘        │
//   TCP :4242 ◄──HDLC── sendTo() ◄─────────────────────────────┤
//   RF ◄── radioTask ◄── txRing ◄───────────────────────────────┘
//
//  The rings carry raw RNS packets (one item = one packet, <= ~508 B).
//  Transport routes them per Reticulum's rules and each interface's mode;
//  packet payloads stay end-to-end encrypted between the peers.
// ============================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <esp_heap_caps.h>

#include "Config.h"
#include "Settings.h"
#include "WifiManager.h"
#include "RetiTransportServer.h"
#include "LoRaRadio.h"
#include "Display.h"
#include "RnsAnnounce.h"
#include "LxmfInbox.h"
#include "RnsAdmin.h"
#include "RnsTransport.h"
#include "SdCard.h"
#include "StoreHome.h"
#include "AutoInterface.h"
#include "Power.h"
#include "Pmu.h"
#include "Gps.h"
#include "Diag.h"
#include "LocalLink.h"
#include "Bootloader.h"
#include "Buzzer.h"
#include "Bq25896.h"
#include "Imu.h"
#include <Wire.h>
#include "Leds.h"
#include "Maintenance.h"
#include "ConsoleServer.h"
#include "PppUart.h"
#include "OtaDevice.h"
#include "OtaUpdate.h"

NodeStats g_stats;

// Ring buffer storage lives in PSRAM where the board has any (only tasks touch
// the rings; the radio ISR just posts a notification). The control blocks stay
// internal.
//
// Where there is no PSRAM the storage comes out of the internal heap, and that
// is worth saying: the fallback used to happen in silence, and a Heltec
// Wireless Stick was paying 26916 B for these three rings while every figure
// it reported said it had heap to spare (Diag.h). It also used to allocate the
// control block before it knew whether the storage existed, and leak it on the
// way past — a few hundred bytes of internal RAM, on precisely the boards that
// have none to lose.
static bool sRingsInPsram = true;

static RingbufHandle_t psramRing(size_t bytes) {
  if (uint8_t* storage = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
    if (auto* cb = (StaticRingbuffer_t*)heap_caps_malloc(sizeof(StaticRingbuffer_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
      return xRingbufferCreateStatic(bytes, RINGBUF_TYPE_NOSPLIT, storage, cb);
    heap_caps_free(storage);            // no control block: the storage is no use on its own
  }
  sRingsInPsram = false;
  return xRingbufferCreate(bytes, RINGBUF_TYPE_NOSPLIT);
}

// The directions of the bridge. NOSPLIT keeps every item (= one RNS
// packet) contiguous, so consumers get a plain pointer + length.
static RingbufHandle_t txRing = nullptr;   // TCP  -> LoRa
static RingbufHandle_t rxRing = nullptr;   // LoRa -> Transport
static RingbufHandle_t tcpInRing = nullptr; // TCP clients -> Transport

// The ways a host reaches this node (LocalLink.h). Registered before the
// services start so /api/status and the console can list every one from the
// first request, including the ones this board or build cannot offer.
static LocalLink::WifiApLink  apLink;
static LocalLink::WifiStaLink staLink;
#if HAS_USB_NCM
static LocalLink::UsbNcmLink usbLink;
#else
static LocalLink::UnavailableLink usbLink(LocalLink::Type::UsbNcm, "usb0", BOARD_USB_NCM,
  BOARD_USB_NCM ? "this build runs the chip's USB as a serial port, not as the composite device"
                : "this board's USB is a serial bridge, not the chip's own");
#endif
#if HAS_PPP
static LocalLink::PppLink pppLink;
#else
static LocalLink::UnavailableLink pppLink(LocalLink::Type::PppUart, "ppp0", false,
  "this board has no bridge UART to carry PPP");
#endif

// Whether the task that drives Reticulum was created; the transport is only
// "online" if something is running its loop.
static bool sRnsTaskUp = false;

// The version floor an update is judged against, and where it is remembered.
// Opened in setup() rather than here: NVS is not up this early.
static Ota::NvsStore     otaStore;
static Ota::Floor<Ota::NvsStore> otaFloor(otaStore);

void setup() {
  // Prefer PSRAM for anything larger than a few hundred bytes — packet
  // buffers, JSON documents, strings, Transport containers. Done first so
  // every later allocation benefits.
  if (psramFound()) heap_caps_malloc_extmem_enable(PSRAM_MALLOC_THRESHOLD);

  #if HAS_PPP
    // The port is shared with PPP (PppUart.h): the driver's receive ring is
    // PPP's receive ring, and a transmit queue lets a whole frame — or a
    // whole console reply — leave in one write. Both are sized before the
    // driver is installed, which is the only time they can be: the switch
    // cannot give them back later, so a board that carries PPP pays for the
    // rings whether or not it is switched on. What the switch does give back
    // is the interface and the reader task. Neither size may be zero, and a
    // smaller pair for the console alone is not on offer for the same reason:
    // setTxBufferSize(0) does not mean unbuffered, it means the driver never
    // installs and the port goes silent — no console and no log at all.
    Serial.setRxBufferSize(PPP_RX_RING_BYTES);
    Serial.setTxBufferSize(PPP_TX_QUEUE_BYTES);
  #endif
  Serial.begin(115200);
  #if HAS_USB_NCM
    // On the composite device the log rides the ACM port with the console,
    // as it rode the USB-Serial/JTAG port before. The core routes it there
    // for the JTAG unit on its own and for the OTG CDC only when asked.
    Serial.setDebugOutput(true);
    // And the port's own reboot — the 1200-baud touch, esptool's DTR/RTS
    // pattern — is off from here, the first moment the sketch has: the core
    // would otherwise restart into the ROM from inside its USB task, past
    // the sequencer, the detach and the marks (UsbNcm.cpp, onLineCoding).
    Serial.enableReboot(false);
  #endif
  delay(300);                              // let the USB CDC host attach
  log_i("%s %s on %s (IDF %s)", FW_NAME, FW_VERSION, BOARD_NAME, esp_get_idf_version());

  // Before anything else that could itself fail: read why the last run ended.
  // The reset register survives the reboot but not a second one, so it is only
  // ever readable here.
  Diag::begin();
  // From here every subsystem says what it cost in the RAM that decides
  // (Diag.h). The bill is per board and is read, not estimated.
  Diag::costStart();

  Leds::begin();                           // claimed and off until the services are up

  // Filesystem first — the web app and the bulletin board live here.
  if (!LittleFS.begin(true)) {
    log_e("LittleFS mount failed even after format");
  }
  Diag::cost("littlefs");

  settings.load();                         // NVS: radio channel, AP, admin
  Diag::cost("settings");
  #if HAS_PMU
    // Before anything touches SPI or I2C: on boards with a power-management
    // chip the transceiver and display rails come up off, so probing the
    // radio first would simply find nothing.
    Pmu::begin();
  #endif
  Power::begin();                          // profile (CPU clock, Wi-Fi sleep) + battery gauge
  Diag::cost("power");

  txRing = psramRing(TX_RING_BYTES);
  rxRing = psramRing(RX_RING_BYTES);
  tcpInRing = psramRing(TCP_IN_RING_BYTES);
  log_i("packet rings: tx %u, rx %u, tcp-in %u B in %s", (unsigned)TX_RING_BYTES,
        (unsigned)RX_RING_BYTES, (unsigned)TCP_IN_RING_BYTES,
        sRingsInPsram ? "PSRAM" : "internal RAM (this board has none)");
  {
    // Both figures, because only one of them can hold a task stack and it is
    // not the larger one (Diag.h). The rings themselves are PSRAM's where
    // there is PSRAM, which is why this line is worth reading beside the cost
    // of the subsystem that follows it.
    const Diag::Heap h = Diag::heap();
    log_i("memory: %lu B internal free (%lu of it 8-bit, %lu largest block), %lu B PSRAM free "
          "(threshold %d B)",
          (unsigned long)h.freeInternal, (unsigned long)h.freeDram, (unsigned long)h.largestDramBlock,
          (unsigned long)h.freePsram, PSRAM_MALLOC_THRESHOLD);
  }
  configASSERT(txRing && rxRing && tcpInRing);
  Diag::cost("packet rings");

  nodeIdentity.begin();                    // Reticulum identity keys (NVS)
  Diag::cost("identity");

  // Bring up services. Radio failure is survivable: the AP + web UI stay
  // up and report "radio offline" so the node can be diagnosed in place.
  g_stats.displayPresent = display.begin(); // probes I2C; clears the panel if found
  Diag::cost("display");
#if HAS_BQ25896 || HAS_DA217
  // After the display, deliberately: the case's I2C parts sit behind the
  // switched peripheral rail, and the panel is what brings that rail up and
  // settles it (Panel.h). Powering it a second time from here left the panel
  // dark — one owner for the rail, and the probe simply comes later.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);  // both residents are 400 kHz parts
  Bq25896::begin();
  Imu::begin();
  Diag::cost("i2c case parts");
#endif

  #if HAS_SD
    // Before the card task exists: it is the task that reads the card's
    // ownership marker for everyone else, and it needs somewhere to put it.
    StoreHome::begin();
    sdCard.begin();                        // mounts; nothing polls the slot yet
    // A move of the store asked for before the last restart happens here, and
    // here specifically: after the card is mounted and before anything else in
    // the node is alive. It is seconds of solid filesystem work, and it used to
    // run further down, by which time the web server was answering requests and
    // the card task was polling the same card. Both of those reach into state
    // this is in the middle of changing, and the node died in the attempt often
    // enough that a migration reliably cost two boots instead of one — visible
    // only when nobody had a serial monitor attached, because watching it
    // changed the timing enough to hide it.
    // The node's name before the move, not after: the migration writes it onto
    // the card it is claiming, and this used to happen inside the Wi-Fi
    // start-up further down, so a card adopted at boot got an owner with no
    // name on it.
    wifiManager.resolveNames();
    StoreHome::runPendingMigration();
  #endif
  // Where the store lives is settled here, for every board and whether or not
  // the transport is switched on. It used to be decided inside the transport's
  // own start-up, behind its enabled check, so a node with the transport off
  // never decided at all: the store's filesystem stayed pointed at flash while
  // the data sat on the card, every page and API answer said so, and an operator
  // acting on that could have the card's real store overwritten by the copy in
  // flash at the next boot.
  StoreHome::chooseAtBoot();
  #if HAS_SD
    // Only now: everything above drives the card directly, and the poll's
    // removal check answers a read it lost to that traffic by unmounting the
    // card out from under whoever is using it.
    sdCard.startPolling();                 // optional; hot-plug polled on core 0
    Diag::cost("sd card");
  #endif
  #if HAS_GPS
    Gps::begin();                          // NMEA reader task; powers the receiver rail
    Diag::cost("gps");
  #endif
  LocalLink::add(&apLink);
  LocalLink::add(&staLink);
  LocalLink::add(&usbLink);
  LocalLink::add(&pppLink);
  // The one to watch, and it bills itself in two parts: the radio the switch
  // is meant to buy, and the web server every node pays for either way.
  wifiManager.begin();                     // radio + web server, or web server alone with Wi-Fi off
  // Before the links, not after: PPP hands the console its own reader when
  // it takes the port (PppUart.h), and a begin() that ran afterwards would
  // point the console back at the UART the reader is already draining —
  // output fine, nothing ever read.
  // The maintenance console on the port the host already has. Its HELLO is
  // the line a flashing tool looks for, so it goes out before the services
  // that make the log busy. Where PPP shares the port, the console reads
  // and writes through the PPP driver's view of it, which hands the console
  // its bytes while the console owns the port and PPP its frames otherwise.
  // The UART directly: with PPP off there is no reader between the two, and
  // when PPP takes the port over it points the console at its own stream
  // (PppUart.h).
  Maintenance::begin(Serial);

  LocalLink::begin();
  Diag::cost("local links");
  g_stats.radioOnline = loraRadio.begin(txRing, rxRing, settings.radio());
  Diag::cost("lora radio");
  // Transport first, then the things that hand it peers. It builds the queue
  // those peers are announced on, and it takes long enough — mounting and
  // walking the store — that a client which was connected before the restart
  // reconnects inside the gap. Accepting first meant that client's arrival was
  // posted to a queue that did not exist yet.
  g_stats.transportOnline = RnsTransport::begin(txRing, rxRing, tcpInRing);
  Diag::cost("reticulum");
  transportServer.begin(tcpInRing);
  Diag::cost("rns tcp server");
  #if HAS_AUTOINTERFACE
    // Zero-config peering on the Wi-Fi links (RNS AutoInterface). Always
    // begun, even with Wi-Fi off: the heartbeat and /api/status ask it for a
    // peer count, and the lock they take exists only once begin() has run. It
    // decides for itself whether there is a netif worth joining.
    AutoInterface::begin(tcpInRing);
    Diag::cost("autointerface");
  #endif

  // ---- Task layout (see the diagram above) -------------------------------
  Diag::startTask(LoRaRadio::radioTask, "radio", 8192, &loraRadio, 5, 1);

  // 6 KB: the panel driver and the I2C stack are deep enough that 4 KB left
  // only ~700 bytes on a T-Beam, where the battery reading adds a PMU
  // transaction to every network page.
  #if HAS_DISPLAY
    // The LVGL shell renders whole widget trees on this stack; the page
    // stack's 6 KB starved it in the first bench build.
    // 16 KB with the GUI: a settings form builds forty widgets inside one
    // click callback inside lv_timer_handler, and 12 KB was the first
    // suspect when that tap took the node down.
    Diag::startTask(Display::displayTask, "display", HAS_LVGL_UI ? 16384 : 6144, &display, 1, 0);
  #endif

  // The RNS task owns every call into microReticulum (Transport is
  // single-threaded): interface loops, forwarding, announces, persistence.
  sRnsTaskUp = Diag::startTask([](void*) {
    // A backstop, and only that. RnsTransport::loop() now guards both halves
    // of its own pass and each of those catches everything, so nothing the
    // library throws reaches here any more — what is left is a throw from the
    // reporting itself, or from a future edit to loop() outside either guard.
    // Kept because the cost is nothing and the one task that must keep running
    // must not be ended by an allocation (Diag.h); described honestly because
    // a reader working out which layer contains what should not be sent to
    // this one.
    for (;;) { Diag::guard("the rns task", [] { RnsTransport::loop(); }); vTaskDelay(pdMS_TO_TICKS(10)); }
  }, "rns", 16384, nullptr, 3, 1);
  // Online means Reticulum is both initialised and being driven: begin()
  // succeeding says only that the tables were built, and a node with no task
  // running its loop routes nothing and announces nothing while every status
  // it serves says "online".
  if (!sRnsTaskUp) {
    g_stats.transportOnline = false;
    log_e("Reticulum is not being driven: the rns task did not start");
  }

  // Last, so the figure beside it is what the node has left to run on.
  Diag::cost("tasks");

  // Everything is up, which is the bar an updated image has to clear: the
  // bootloader started it on approval and puts the previous one back on the
  // next boot unless it is told, here, that this one works. Saying so also
  // settles the version floor — the same statement, recorded for the next
  // update to be judged against. On a first boot after a cable flash there is
  // nothing pending and nothing staged, and this does nothing at all.
  otaStore.begin();
  Ota::begin(otaFloor);
  {
    const Ota::Settlement s = Ota::confirmBoot(otaFloor);
    if (s.advanced) log_i("update confirmed: the version floor is now %lu", (unsigned long)s.floor);
    if (!Ota::canSelfUpdate())
      log_w("this board has a single app partition: it cannot install its own updates, "
            "and an update needs a cable");
  }

  // The audible version of the lines below, for a device carried rather than
  // benched: two notes up means the node is running before the panel says so.
  Buzzer::begin();
  Buzzer::boot();

  if (settings.links().wifiEnabled)
    log_i("RetiMesh Node up — join \"%s\", portal http://%s, RNS TCP :%d",
          wifiManager.ssid(), AP_IP.toString().c_str(), RNS_TCP_PORT);
  else
    log_i("RetiMesh Node up — Wi-Fi is off; HTTP :%d and RNS TCP :%d answer on any other local link, "
          "and WIFI ON at the console turns the access point back on", HTTP_PORT, RNS_TCP_PORT);
}

// Arduino's loopTask (core 1, prio 1): scheduled restarts, the maintenance
// console, link bookkeeping and a heartbeat.
void loop() {
  static uint32_t lastBeat = 0;
  wifiManager.tick();
  Bootloader::tick();                      // may not return: this is where restarts happen
  // Before the console reads: this is what hands it a network session when
  // one arrives, and takes it back when the caller goes (ConsoleServer.h).
  ConsoleServer::poll();
  Maintenance::poll();
  LocalLink::poll(millis());
  // Messages that arrived on the Reticulum task, written here. Flash is slow
  // enough that doing it where they arrive delayed the delivery proof and
  // stalled the job loop that link and receipt timeouts run on (LxmfInbox.h).
  Rns::Inbox::poll();
  // A command that arrived as a message, run here rather than in the packet
  // callback it landed in: it means the console's parser, the settings, and
  // sometimes a restart (RnsAdmin.h).
  Rns::Admin::poll();
  Leds::tick(millis());
  // Every pass, not on the heartbeat: a crash 29 s after the last beat would
  // otherwise be recorded as having happened 29 s earlier, and a node stuck in
  // a restart loop would report every run as zero seconds — indistinguishable
  // from one that never got past setup(). One word into RTC RAM, no flash.
  Diag::tick(millis() / 1000);
  g_stats.heapMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  g_stats.psramFree   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (millis() - lastBeat >= 30000) {
    lastBeat = millis();
    const uint32_t uptimeS = millis() / 1000;
    log_i("up %lus | peers tcp %u auto %u | lora rx/tx %u/%u (drop %u) | announces rx/tx %u/%u",
          (unsigned long)uptimeS, (unsigned)g_stats.tcpClients, (unsigned)AutoInterface::peerCount(),
          (unsigned)g_stats.loraRxPackets, (unsigned)g_stats.loraTxPackets,
          (unsigned)(g_stats.loraRxDropRing + g_stats.loraRxDropReasm +
                     g_stats.loraRxDropPartial), (unsigned)g_stats.announcesRx,
          (unsigned)g_stats.announcesTx);
    // Where the losses went. Silent unless there are some: on a quiet channel
    // this line never appears, and when it does the five figures say which of
    // the five causes is responsible rather than leaving one number to guess at.
    // Spurious interrupts are deliberately not in this sum: nothing was lost
    // when one arrives, TxDone shares the line so a steady trickle is normal,
    // and including them made the loss report fire on an idle node.
    const uint32_t lost = g_stats.loraRxDropRing + g_stats.loraRxDropReasm +
                          g_stats.loraRxDropPartial + g_stats.loraRxCrcErrors +
                          g_stats.loraRxBadLength;
    if (lost)
      log_i("lora rx losses: ring %u, reassembly %u, partial %u, crc %u, length %u, "
            "spurious irq %u (kept %u)",
            (unsigned)g_stats.loraRxDropRing, (unsigned)g_stats.loraRxDropReasm,
            (unsigned)g_stats.loraRxDropPartial, (unsigned)g_stats.loraRxCrcErrors,
            (unsigned)g_stats.loraRxBadLength, (unsigned)g_stats.loraRxSpuriousIrq,
            (unsigned)g_stats.loraRxPackets);
    // Reticulum's tables are the other thing that grows with traffic, and the
    // one a heap figure alone will not explain.
    RnsTransport::Tables t = RnsTransport::tables();
    log_i("tables: paths %lu links %lu (%lu active, %lu pending) dests %lu announces %lu (%lu held) rates %lu",
          (unsigned long)t.paths, (unsigned long)t.links, (unsigned long)t.activeLinks,
          (unsigned long)t.pendingLinks, (unsigned long)t.destinations,
          (unsigned long)t.announces, (unsigned long)t.heldAnnounces, (unsigned long)t.rates);
    #if HAS_GPS
      // The satellite count is the number that tells you whether the antenna
      // has a view of the sky; the sentence count tells you the receiver is
      // wired up at all.
      Gps::Fix g = Gps::fix();
      if (g.enabled)
        log_i("gnss: %s, %u sats, %lu sentences%s%s", g.valid ? "fix" : "searching",
              g.satellites, (unsigned long)g.sentences,
              g.timeValid ? ", utc " : "", g.timeValid ? g.utc : "");
    #endif
    // Heap, per-task stack headroom and the low-water warnings. The task list
    // lives in Diag.h so this and /api/status can never watch different sets —
    // the GNSS task, which carries the smallest stack of the lot, was missing
    // from the copy that used to live here.
    Diag::report();
  }
  vTaskDelay(pdMS_TO_TICKS(200));
}
