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
//  │  dnsTask                captive-portal DNS polling         │
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
#include "Maintenance.h"

NodeStats g_stats;

// Ring buffer storage lives in PSRAM (only tasks touch the rings; the radio
// ISR just posts a notification). The control blocks stay internal.
static RingbufHandle_t psramRing(size_t bytes) {
  uint8_t* storage = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  StaticRingbuffer_t* cb = (StaticRingbuffer_t*)heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!storage || !cb) return xRingbufferCreate(bytes, RINGBUF_TYPE_NOSPLIT);   // no PSRAM: internal
  return xRingbufferCreateStatic(bytes, RINGBUF_TYPE_NOSPLIT, storage, cb);
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
static LocalLink::UnavailableLink usbLink(LocalLink::Type::UsbNcm, "usb0", BOARD_USB_NCM,
  BOARD_USB_NCM ? "this build has no USB network stack (the core's TinyUSB carries NCM; the firmware does not drive it yet)"
                : "this board's USB is a serial bridge, not the chip's own");
static LocalLink::UnavailableLink pppLink(LocalLink::Type::PppUart, "ppp0", BOARD_UART_NETWORK,
  BOARD_UART_NETWORK ? "this build has no PPP (the core's lwIP has it; the firmware does not drive it yet)"
                     : "this board has no bridge UART to carry PPP");

void setup() {
  // Prefer PSRAM for anything larger than a few hundred bytes — packet
  // buffers, JSON documents, strings, Transport containers. Done first so
  // every later allocation benefits.
  if (psramFound()) heap_caps_malloc_extmem_enable(PSRAM_MALLOC_THRESHOLD);

  Serial.begin(115200);
  delay(300);                              // let the USB CDC host attach
  log_i("%s %s on %s (IDF %s)", FW_NAME, FW_VERSION, BOARD_NAME, esp_get_idf_version());

  // Before anything else that could itself fail: read why the last run ended.
  // The reset register survives the reboot but not a second one, so it is only
  // ever readable here.
  Diag::begin();

  #if PIN_STATUS_LED >= 0
    // Claimed and held off. No feature drives it yet, but leaving a wired pin
    // floating invites it to do something the firmware never asked for.
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);
  #endif

  // Filesystem first — the web app and the bulletin board live here.
  if (!LittleFS.begin(true)) {
    log_e("LittleFS mount failed even after format");
  }

  settings.load();                         // NVS: radio channel, AP, admin
  #if HAS_PMU
    // Before anything touches SPI or I2C: on boards with a power-management
    // chip the transceiver and display rails come up off, so probing the
    // radio first would simply find nothing.
    Pmu::begin();
  #endif
  Power::begin();                          // profile (CPU clock, Wi-Fi sleep) + battery gauge

  txRing = psramRing(TX_RING_BYTES);
  rxRing = psramRing(RX_RING_BYTES);
  tcpInRing = psramRing(TCP_IN_RING_BYTES);
  log_i("memory: internal %u free, PSRAM %u free (threshold %d B)",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), PSRAM_MALLOC_THRESHOLD);
  configASSERT(txRing && rxRing && tcpInRing);

  nodeIdentity.begin();                    // Reticulum identity keys (NVS)

  // Bring up services. Radio failure is survivable: the AP + web UI stay
  // up and report "radio offline" so the node can be diagnosed in place.
  g_stats.displayPresent = display.begin(); // probes I2C; clears the panel if found
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
  #endif
  #if HAS_GPS
    Gps::begin();                          // NMEA reader task; powers the receiver rail
  #endif
  LocalLink::add(&apLink);
  LocalLink::add(&staLink);
  LocalLink::add(&usbLink);
  LocalLink::add(&pppLink);
  wifiManager.begin();                     // radio + web server, or web server alone with Wi-Fi off
  LocalLink::begin();
  // The maintenance console on the port the host already has. Its HELLO is
  // the line a flashing tool looks for, so it goes out before the services
  // that make the log busy.
  Maintenance::begin(Serial);
  transportServer.begin(tcpInRing);
  g_stats.radioOnline = loraRadio.begin(txRing, rxRing, settings.radio());
  g_stats.transportOnline = RnsTransport::begin(txRing, rxRing, tcpInRing);
  #if HAS_AUTOINTERFACE
    // Zero-config peering on the AP (RNS AutoInterface). Always begun, even
    // with Wi-Fi off: the heartbeat and /api/status ask it for a peer count,
    // and the lock they take exists only once begin() has run. It decides
    // for itself whether there is a netif worth joining.
    AutoInterface::begin(tcpInRing);
  #endif

  // ---- Task layout (see the diagram above) -------------------------------
  // The captive-portal DNS only exists to steer phones on the access point.
  // Without one there is no server to poll — and polling a DNSServer that
  // was never started costs a malloc and a logged error every ten
  // milliseconds, on the same port the maintenance console answers on.
  if (wifiManager.wifiEnabled())
    xTaskCreatePinnedToCore(WifiManager::dnsTask, "dns",
                            3072, &wifiManager, 1, nullptr, 0);

  xTaskCreatePinnedToCore(LoRaRadio::radioTask, "radio",
                          8192, &loraRadio, 5, nullptr, 1);

  // 6 KB: the panel driver and the I2C stack are deep enough that 4 KB left
  // only ~700 bytes on a T-Beam, where the battery reading adds a PMU
  // transaction to every network page.
  #if HAS_DISPLAY
    xTaskCreatePinnedToCore(Display::displayTask, "display",
                            6144, &display, 1, nullptr, 0);
  #endif

  // The RNS task owns every call into microReticulum (Transport is
  // single-threaded): interface loops, forwarding, announces, persistence.
  xTaskCreatePinnedToCore([](void*) {
    for (;;) { RnsTransport::loop(); vTaskDelay(pdMS_TO_TICKS(10)); }
  }, "rns", 16384, nullptr, 3, nullptr, 1);

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
  Maintenance::poll();
  LocalLink::poll(millis());
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
