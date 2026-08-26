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

#include "Config.h"
#include "Settings.h"
#include "WifiManager.h"
#include "RetiTransportServer.h"
#include "LoRaRadio.h"
#include "Display.h"
#include "RnsAnnounce.h"
#include "RnsTransport.h"
#include "SdCard.h"

NodeStats g_stats;

// The two directions of the bridge. NOSPLIT keeps every item (= one RNS
// packet) contiguous, so consumers get a plain pointer + length.
static RingbufHandle_t txRing = nullptr;   // TCP  -> LoRa
static RingbufHandle_t rxRing = nullptr;   // LoRa -> Transport
static RingbufHandle_t tcpInRing = nullptr; // TCP clients -> Transport

void setup() {
  Serial.begin(115200);
  delay(300);                              // let the USB CDC host attach
  log_i("%s %s booting (IDF %s)", FW_NAME, FW_VERSION, esp_get_idf_version());

  // Filesystem first — the web app and the bulletin board live here.
  if (!LittleFS.begin(true)) {
    log_e("LittleFS mount failed even after format");
  }

  settings.load();                         // NVS: radio channel, AP, admin

  txRing = xRingbufferCreate(TX_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  rxRing = xRingbufferCreate(RX_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  tcpInRing = xRingbufferCreate(TCP_IN_RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  configASSERT(txRing && rxRing && tcpInRing);

  nodeIdentity.begin();                    // Reticulum identity keys (NVS)

  // Bring up services. Radio failure is survivable: the AP + web UI stay
  // up and report "radio offline" so the node can be diagnosed in place.
  g_stats.displayPresent = display.begin(); // probes I2C; clears the panel if found
  #if HAS_SD
    sdCard.begin();                        // optional; hot-plug polled on core 0
  #endif
  wifiManager.begin();
  transportServer.begin(tcpInRing);
  g_stats.radioOnline = loraRadio.begin(txRing, rxRing, settings.radio());
  g_stats.transportOnline = RnsTransport::begin(txRing, rxRing, tcpInRing);

  // ---- Task layout (see the diagram above) -------------------------------
  xTaskCreatePinnedToCore(WifiManager::dnsTask, "dns",
                          3072, &wifiManager, 1, nullptr, 0);

  xTaskCreatePinnedToCore(LoRaRadio::radioTask, "radio",
                          8192, &loraRadio, 5, nullptr, 1);

  xTaskCreatePinnedToCore(Display::displayTask, "display",
                          4096, &display, 1, nullptr, 0);

  // The RNS task owns every call into microReticulum (Transport is
  // single-threaded): interface loops, forwarding, announces, persistence.
  xTaskCreatePinnedToCore([](void*) {
    for (;;) { RnsTransport::loop(); vTaskDelay(pdMS_TO_TICKS(10)); }
  }, "rns", 16384, nullptr, 3, nullptr, 1);

  log_i("RetiMesh Node up — join \"%s\", portal http://10.42.0.1, RNS TCP :%d",
        wifiManager.ssid(), RNS_TCP_PORT);
}

// Arduino's loopTask (core 1, prio 1): scheduled restarts + a heartbeat.
void loop() {
  static uint32_t lastBeat = 0;
  wifiManager.tick();
  if (millis() - lastBeat >= 30000) {
    lastBeat = millis();
    log_i("up %lus | rns peers %u | lora rx/tx %u/%u (drop %u) | beacons rx/tx %u/%u | heap %u",
          millis() / 1000, (unsigned)g_stats.tcpClients,
          (unsigned)g_stats.loraRxPackets, (unsigned)g_stats.loraTxPackets,
          (unsigned)g_stats.loraRxDropped, (unsigned)g_stats.beaconsRx,
          (unsigned)g_stats.beaconsTx, (unsigned)ESP.getFreeHeap());
  }
  vTaskDelay(pdMS_TO_TICKS(200));
}
