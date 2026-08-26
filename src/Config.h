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
//  Config.h — compile-time configuration for the RetiMesh Node
//
//  Everything here can be overridden from platformio.ini build_flags
//  (every #define is guarded by #ifndef).
// ============================================================================
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Firmware version — single-sourced from the git tag by CI
// (PLATFORMIO_BUILD_FLAGS=-DFW_VERSION=\"v1.2.3\"); local builds say "dev".
// ---------------------------------------------------------------------------
#ifndef FW_VERSION
  #define FW_VERSION        "dev"
#endif
#define FW_NAME             "RetiMesh Node"

// ---------------------------------------------------------------------------
// Wi-Fi SoftAP
// ---------------------------------------------------------------------------
// SSID = AP_SSID_PREFIX + "-" + last three octets of the factory MAC,
// e.g. "retimesh-A1B2C3", so several nodes in range never collide.
// Define AP_SSID to force a fixed name instead.
#ifndef AP_SSID_PREFIX
  #define AP_SSID_PREFIX    "retimesh"
#endif
#ifndef AP_PASSWORD
  #define AP_PASSWORD       ""              // empty = open network
#endif
#ifndef AP_CHANNEL
  #define AP_CHANNEL        6
#endif
#ifndef AP_MAX_STATIONS
  #define AP_MAX_STATIONS   8
#endif

// AP address 10.42.0.1 — "42" as a nod to the transport port.
#define AP_IP               IPAddress(10, 42, 0, 1)
#define AP_NETMASK          IPAddress(255, 255, 255, 0)

// ---------------------------------------------------------------------------
// Network services
// ---------------------------------------------------------------------------
#ifndef HTTP_PORT
  #define HTTP_PORT         80
#endif
#ifndef RNS_TCP_PORT
  #define RNS_TCP_PORT      4242
#endif
#ifndef RNS_MAX_CLIENTS
  #define RNS_MAX_CLIENTS   4               // simultaneous Sideband/RNS peers
#endif

// ---------------------------------------------------------------------------
// Reticulum sizes
// ---------------------------------------------------------------------------
// RNS.Reticulum.MTU — the largest packet a Reticulum instance emits.
#define RNS_MTU             500

// Physical LoRa framing (RNode-compatible, see LoRaRadio.cpp):
// each RF frame is 1 header byte + up to 254 payload bytes.
#define LORA_FRAME_MAX      255
#define LORA_HEADER_LEN     1
#define LORA_FRAG_PAYLOAD   (LORA_FRAME_MAX - LORA_HEADER_LEN)   // 254
#define LORA_FLAG_SPLIT     0x01
#define LORA_SEQ_UNSET      0xFF

// ---------------------------------------------------------------------------
// LoRa radio on SPI (defaults: LilyGO T3-S3 v1.2/v1.3). The board exists
// with an SX1262 (DIO1 33, BUSY 34) or an SX1276/78 (DIO0 9, DIO1 33);
// LoRaRadio::begin() probes both, so the same build serves either.
// ---------------------------------------------------------------------------
#ifndef PIN_LORA_SCK
  #define PIN_LORA_SCK      5
#endif
#ifndef PIN_LORA_MISO
  #define PIN_LORA_MISO     3
#endif
#ifndef PIN_LORA_MOSI
  #define PIN_LORA_MOSI     6
#endif
#ifndef PIN_LORA_CS
  #define PIN_LORA_CS       7
#endif
#ifndef PIN_LORA_RST
  #define PIN_LORA_RST      8
#endif
#ifndef PIN_LORA_BUSY
  #define PIN_LORA_BUSY     34
#endif
#ifndef PIN_LORA_DIO1
  #define PIN_LORA_DIO1     33
#endif
#ifndef PIN_LORA_DIO0
  #define PIN_LORA_DIO0     9               // SX127x only
#endif

// PHY parameters. These MUST match every other node on the channel
// (including real RNodes — configure the RNode side with the same values).
#ifndef RF_FREQ_MHZ
  #define RF_FREQ_MHZ       869.525         // EU868 SRD band, 500 mW sub-band
#endif
#ifndef RF_BW_KHZ
  #define RF_BW_KHZ         125.0
#endif
#ifndef RF_SF
  #define RF_SF             8
#endif
#ifndef RF_CR
  #define RF_CR             5               // 4/5
#endif
#ifndef RF_TX_DBM
  #define RF_TX_DBM         17              // SX1262 max is 22
#endif
#ifndef RF_PREAMBLE_SYMS
  #define RF_PREAMBLE_SYMS  18              // RNode's LORA_PREAMBLE_SYMBOLS_MIN
#endif
// 0x12 is the classic SX127x "private network" sync word; RadioLib
// translates it to the equivalent SX126x two-byte value (0x1424).
#ifndef RF_SYNCWORD
  #define RF_SYNCWORD       0x12
#endif
// T3-S3 SX1262 modules have a TCXO fed from DIO3. Set to 0.0 for
// plain-crystal modules (begin() fails with -706/-707 when this is wrong).
#ifndef RF_TCXO_VOLTAGE
  #define RF_TCXO_VOLTAGE   1.8
#endif
// The T3-S3 routes its RF switch from DIO2.
#ifndef RF_DIO2_AS_SWITCH
  #define RF_DIO2_AS_SWITCH true
#endif

// ---------------------------------------------------------------------------
// OLED — SSD1306 128x64 on I2C (T3-S3: SDA 18 / SCL 17, address 0x3C)
// ---------------------------------------------------------------------------
#ifndef HAS_DISPLAY
  #define HAS_DISPLAY       1
#endif
#ifndef PIN_OLED_SDA
  #define PIN_OLED_SDA      18
#endif
#ifndef PIN_OLED_SCL
  #define PIN_OLED_SCL      17
#endif
#ifndef OLED_ADDR
  #define OLED_ADDR         0x3C
#endif
#ifndef OLED_ROTATION
  #define OLED_ROTATION     0               // 0..3, quarter turns
#endif
#define DISPLAY_REFRESH_MS  500

// ---------------------------------------------------------------------------
// CSMA (simplified: CAD check + random slotted backoff — see LoRaRadio.cpp)
// ---------------------------------------------------------------------------
#define CSMA_SLOT_MS        10
#define CSMA_MAX_SLOTS      8
#define CSMA_MAX_WAIT_MS    2000            // give up deferring after this

// ---------------------------------------------------------------------------
// Ring buffers bridging TCP <-> LoRa (created in main.cpp)
// ---------------------------------------------------------------------------
#define TX_RING_BYTES       8192            // TCP  -> LoRa direction
#define RX_RING_BYTES       8192            // LoRa -> TCP  direction

// ---------------------------------------------------------------------------
// Bulletin board
// ---------------------------------------------------------------------------
#define BOARD_FILE          "/board.json"
#define BOARD_MAX_POSTS     50
#define BOARD_MAX_TEXT      280
#define BOARD_MAX_AUTHOR    32

// ---------------------------------------------------------------------------
// Shared runtime stats (single writer per field, plain volatile is enough)
// ---------------------------------------------------------------------------
struct NodeStats {
  volatile bool     radioOnline   = false;
  const char*       radioModel    = "none"; // "SX1262" / "SX1276" once probed
  volatile float    lastRssi      = 0.0f;   // dBm, last LoRa RX
  volatile float    lastSnr       = 0.0f;   // dB,  last LoRa RX
  volatile uint32_t loraRxPackets = 0;      // reassembled RNS packets
  volatile uint32_t loraTxPackets = 0;
  volatile uint32_t loraRxDropped = 0;      // ring-full / oversize drops
  volatile uint32_t tcpRxPackets  = 0;      // deframed packets from clients
  volatile uint32_t tcpClients    = 0;
};

extern NodeStats g_stats;
