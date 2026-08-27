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
// Board selection. One header per board under boards/ defines the pin map and
// the capability flags (HAS_SD, HAS_PMU, HAS_GPS, ...); everything below is
// guarded by #ifndef, so a board header wins and anything it leaves out falls
// back to the general default. Pass -DBOARD_TBEAM (or another BOARD_*) from
// platformio.ini; without one, the T3-S3 is assumed.
// ---------------------------------------------------------------------------
#if defined(BOARD_TBEAM)
  #include "boards/tbeam.h"
#elif defined(BOARD_T3S3_SX1280_PA)
  #include "boards/t3s3_sx1280_pa.h"
#elif defined(BOARD_T3S3_SX1280)
  #include "boards/t3s3_sx1280.h"
#else
  #include "boards/t3s3.h"
#endif

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
// SoftAP WPA3 (SAE) needs ESP-IDF 5; the pinned Arduino core 2.x (IDF 4.4)
// rejects the auth mode (verified: esp_wifi_set_config -> ESP_ERR_INVALID_ARG,
// AP stays WPA2). The code path is kept for an IDF 5 core.
#include <esp_idf_version.h>
#define WPA3_SOFTAP_SUPPORTED (ESP_IDF_VERSION_MAJOR >= 5)

// Default security when no setting is stored: 0 open, 1 WPA2,
// 2 WPA2+WPA3 mixed, 3 WPA3-only. Ignored unless AP_PASSWORD has >= 8 chars.
#ifndef AP_SECURITY_DEFAULT
  #define AP_SECURITY_DEFAULT 1
#endif
#ifndef AP_CHANNEL
  #define AP_CHANNEL        6
#endif
#ifndef AP_MAX_STATIONS
  #define AP_MAX_STATIONS   8
#endif

// Admin password protecting the settings API/page (HTTP Basic Auth,
// user "admin"). Change it from the settings page; stored in NVS.
#ifndef ADMIN_PASSWORD_DEFAULT
  #define ADMIN_PASSWORD_DEFAULT "retimesh"
#endif
#define NVS_NAMESPACE       "retimesh"

// ---------------------------------------------------------------------------
// Diagnostics (see Diag.h). The boot counter lives in its own NVS namespace so
// a settings reset does not erase the restart history a soak run is built on.
// ---------------------------------------------------------------------------
#define DIAG_NVS_NAMESPACE  "retimesh-diag"     // max 15 chars
#define DIAG_STACK_WARN_B   768                 // headroom below this: name the task
#define DIAG_HEAP_WARN_B    20480               // internal heap low-water below this

#ifndef MDNS_HOSTNAME
  #define MDNS_HOSTNAME     "retimesh"      // http://retimesh.local/
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
// RNS AutoInterface (IPv6 link-local multicast peering on the SoftAP)
// ---------------------------------------------------------------------------
#ifndef HAS_AUTOINTERFACE
  #define HAS_AUTOINTERFACE 1
#endif
#ifndef AUTOIF_GROUP_ID
  #define AUTOIF_GROUP_ID   "reticulum"     // RNS default group id
#endif
#define AUTOIF_MAX_PEERS    8

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
// LoRa radio on SPI. The pin map comes from the board header; these are the
// fallbacks for a board that does not define one. Both the SX1262 lines
// (DIO1/BUSY) and the SX127x line (DIO0) exist because LoRaRadio::begin()
// probes for either transceiver.
// ---------------------------------------------------------------------------
#ifndef LORA_SPI_BUS
  #define LORA_SPI_BUS      FSPI            // general-purpose bus on the ESP32-S3
#endif
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

// PHY defaults — the running values live in NVS (see Settings.h) and are
// editable from the settings page. They MUST match every other node on
// the channel (including real RNodes — configure the RNode side alike).
#ifndef RF_FREQ_MHZ
  #define RF_FREQ_MHZ       869.410         // EU868 SRD band
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
  #define RF_TX_DBM         7               // SX1262 max 22, SX1276 max 17
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
// Set by a board header carrying a 2.4 GHz module; see LoRaRadio::begin().
#ifndef RF_MODEM_SX1280
  #define RF_MODEM_SX1280   0
#endif

// Boards with a transmit/receive switch in front of the antenna name its pins;
// RadioLib steers them. Without one the radio is wired straight through.
#ifndef HAS_RF_SWITCH
  #define HAS_RF_SWITCH     0
#endif

// A board with an external PA radiates more than the chip is asked for, so the
// ceiling belongs to the board rather than to RadioCaps, which describes chips.
// Zero means "whatever the transceiver itself allows".
#ifndef RF_TX_DBM_MAX
  #define RF_TX_DBM_MAX     0
#endif

// A single timed transmission at boot, to prove the IRQ line is the one the
// board actually uses. Off everywhere it is not needed: it costs airtime.
#ifndef RADIO_SELFTEST_ON_BOOT
  #define RADIO_SELFTEST_ON_BOOT 0
#endif

#ifndef RF_TCXO_VOLTAGE
  #define RF_TCXO_VOLTAGE   1.8
#endif
// The T3-S3 routes its RF switch from DIO2.
#ifndef RF_DIO2_AS_SWITCH
  #define RF_DIO2_AS_SWITCH true
#endif

// ---------------------------------------------------------------------------
// microSD on its own SPI bus. Boards without a slot set HAS_SD 0 and the
// whole driver compiles out.
// ---------------------------------------------------------------------------
#ifndef HAS_SD
  #define HAS_SD            1
#endif
#ifndef PIN_SD_MOSI
  #define PIN_SD_MOSI       11
#endif
#ifndef PIN_SD_MISO
  #define PIN_SD_MISO       2
#endif
#ifndef PIN_SD_SCK
  #define PIN_SD_SCK        14
#endif
#ifndef PIN_SD_CS
  #define PIN_SD_CS         13
#endif
#define SD_SPI_HZ           20000000
#define SD_POLL_MS          3000
#define SD_PARTIAL_PERCENT  50            // volume < 50 % of the card => "partial"
#define SD_MOUNT_ATTEMPTS   6             // boot: the first mount after SPI init often fails
#define SD_MOUNT_RETRY_MS   100           // ~0.6 s of retries before giving up

// Web-layer snapshots of the Reticulum path table. Each row is read back
// through microStore (flash or SD), so this is deliberately unhurried.
#define SNAPSHOT_INTERVAL_MS  5000
#define SNAPSHOT_MAX_PATHS    64
#define SD_LOG_MAX_BYTES    (1024UL * 1024UL)

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
// BOOT button (GPIO 0, active low) doubles as the display navigation key:
// short press = next page, long press = blank/wake the panel.
#ifndef PIN_BUTTON
  #define PIN_BUTTON        0
#endif
#define BUTTON_POLL_MS      20
#define BUTTON_LONG_MS      1500
#define DISPLAY_PAGE_TIMEOUT_MS 30000     // back to the status page when idle
#define DISPLAY_SLEEP_MS    60000         // panel off after this much button inactivity

// ---------------------------------------------------------------------------
// Beacons / neighbour discovery (see LoRaRadio.h). Interval in seconds,
// 0 disables. Matches RNS's RNodeInterface `id_interval` semantics closely
// enough that an RNode's `id_callsign` shows up in our neighbour table.
// ---------------------------------------------------------------------------
#ifndef BEACON_INTERVAL_S
  #define BEACON_INTERVAL_S 0             // off by default: announces do this job
#endif
// Reticulum announces of this node's retimesh.node destination, in
// seconds (0 = off). Sent on LoRa and to Wi-Fi clients; on boot as well.
#ifndef ANNOUNCE_INTERVAL_S
  #define ANNOUNCE_INTERVAL_S 600
#endif
#define ANNOUNCE_BOOT_DELAY_MS 6000
#define ANNOUNCE_MAX_LEN    256
#define TCP_IN_RING_BYTES   8192          // TCP clients -> Transport
#define BEACON_MAX_LEN      64            // printable payload bytes
// RetiMesh beacons are valid Reticulum packets: a broadcast to the PLAIN
// destination "retimesh.beacon" (hash = RNS.Destination.hash(None,
// "retimesh", "beacon")). RNS peers accept and silently drop them
// instead of counting a protocol violation, and any RNS program that
// registers that destination receives them. Header = flags, hops, hash,
// context = 19 bytes, followed by the printable beacon text.
#define RNS_BEACON_FLAGS    0x08          // header type 1, broadcast, PLAIN, DATA
#define RNS_BEACON_HDR_LEN  19
#define RNS_BEACON_DEST_HASH { 0xC8, 0x03, 0xD3, 0x5E, 0x39, 0xD0, 0xAA, 0x7A, \
                               0xCD, 0xE2, 0x21, 0xBB, 0x16, 0x7F, 0x05, 0x3E }
#define BEACON_HELLO_DELAY_MS 3000        // boot probe
#define MAX_NEIGHBORS       16
#define NEIGHBOR_STALE_MS   (3UL * 60UL * 1000UL)

// ---------------------------------------------------------------------------
// Power / battery. Boards with a power-management chip (HAS_PMU) read the
// cell through it; the rest use an ADC divider (HAS_BATTERY_ADC).
// ---------------------------------------------------------------------------
#ifndef HAS_PMU
  #define HAS_PMU           0
#endif
#ifndef HAS_GPS
  #define HAS_GPS           0
#endif
#ifndef HAS_BATTERY_ADC
  #define HAS_BATTERY_ADC   1
#endif
#ifndef BOARD_NAME
  #define BOARD_NAME        "unknown board"
#endif
#ifndef PIN_BATTERY_ADC
  #define PIN_BATTERY_ADC   1
#endif
// How much current the PMU may draw from VBUS. This caps what is left for the
// cell once the running node has taken its 120-250 mA, so a low ceiling is why
// a flat cell gains slowly. It stays at 500 mA because that is all an unknown
// USB source is obliged to supply, and drawing more can brown out a laptop
// port. A board or build that knows its supply can raise it — AXP2101 steps
// are 500, 900, 1000, 1500 and 2000 mA; an AXP192 tops out at 500. The input
// *voltage* limit is set alongside it either way, so the chip throttles itself
// if the supply turns out to be weaker than the ceiling allows.
#ifndef PMU_VBUS_LIMIT_MA
  #define PMU_VBUS_LIMIT_MA 500
#endif

#define BATTERY_DIVIDER_RATIO 2.0f
#define BATTERY_MIN_V       3.0f            // below: no cell attached (USB bench)
#define BATTERY_MAX_V       4.35f
#define BATTERY_SAMPLE_MS   10000
#define DISPLAY_SLEEP_BATTERY_MS 20000

// ---------------------------------------------------------------------------
// Optional manual cap on the hourly transmit budget, in whole percent. The
// allowance normally comes from the sub-band the channel sits in (Airtime.h
// carries the EU 863-870 plan), and this only ever tightens it further; 0
// means "whatever the band allows". It is the operator's own limit for a
// channel outside that plan, where the local rules are theirs to know.
#ifndef RF_DUTY_CYCLE_PCT
  #define RF_DUTY_CYCLE_PCT 0
#endif

// CSMA: DIFS + a contention window sized from recent channel use, as in
// RNode. The slot length and window bands come from Airtime.h.
// ---------------------------------------------------------------------------
#define CSMA_MAX_WAIT_MS    5000            // give up deferring after this
#define CSMA_CAD_RETRY_MS   50              // pause between channel probes

// ---------------------------------------------------------------------------
// PSRAM. The core routes only malloc() > 4 KB to PSRAM by default; almost
// nothing here is that large, so internal RAM fills up while 2 MB sit idle.
// Allocations above this threshold prefer PSRAM instead (ISR code never
// allocates, so it is safe); ring buffer storage goes there explicitly.
// ---------------------------------------------------------------------------
#ifndef PSRAM_MALLOC_THRESHOLD
  #define PSRAM_MALLOC_THRESHOLD 128      // measured: 512 B -> +28 KB internal, 128 B -> +42 KB
#endif

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
  volatile bool     transportOnline = false;
  const char*       radioModel    = "none"; // "SX1262" / "SX1276" once probed
  volatile bool     displayPresent = false;
  volatile int16_t  radioApplyError = 0;   // last RadioLib error applying settings
  volatile uint32_t beaconsTx     = 0;
  volatile uint32_t beaconsRx     = 0;
  volatile uint32_t announcesTx   = 0;
  volatile uint32_t announcesRx   = 0;    // verified announces heard (any side)
  volatile uint32_t heapMinFree   = 0;    // lowest free internal heap seen (soak monitoring)
  volatile uint32_t psramFree     = 0;
  volatile float    lastRssi      = 0.0f;   // dBm, last LoRa RX
  volatile float    lastSnr       = 0.0f;   // dB,  last LoRa RX
  volatile uint32_t loraRxPackets = 0;      // reassembled RNS packets
  volatile uint32_t loraTxPackets = 0;
  // Channel use, maintained by the radio task (see Airtime.h)
  volatile float    airtimeShort  = 0.0f;   // 0..1 over the last two minutes
  volatile float    airtimeLong   = 0.0f;   // 0..1 over the rolling hour
  volatile float    dutyBudget    = 0.0f;   // 0..1+ of the hourly allowance
  volatile bool     dutyLocked    = false;  // transmissions held back
  volatile uint32_t dutyRetryS    = 0;      // seconds until the budget frees up
  volatile uint16_t dutyLimitBp   = 0;      // enforced allowance in basis points, 100 = 1 %
  volatile uint16_t csmaSlotMs    = 0;
  volatile uint8_t  csmaBand      = 1;      // contention window band, 1..4
  // Where receptions go when they do not become a packet. One counter for all
  // of them told us a node was losing 94 % of its receptions but not why, and
  // the causes have nothing to do with each other: a full ring means the
  // consumer fell behind, a reassembly overflow means the fragments were
  // wrong, a CRC error means the air was noisy.
  //
  // These do NOT sum to everything heard on their own. A frame is either
  // rejected before decoding (loraRxCrcErrors, loraRxBadLength), consumed as
  // one fragment of a reassembly still in progress, or it completes a packet —
  // and a completed packet lands in loraRxPackets, in beaconsRx when it turns
  // out to be a beacon or an RNode station ID, or in loraRxDropRing when the
  // consumer could not take it. loraRxDropPartial counts reassemblies given up
  // before they completed. Note the units differ: the first two count frames,
  // the rest count packets, because a split packet arrives as two frames.
  volatile uint32_t loraRxDropRing    = 0;  // RX ring full: the RNS task fell behind
  volatile uint32_t loraRxDropReasm   = 0;  // second fragment did not fit the buffer
  volatile uint32_t loraRxDropPartial = 0;  // half-assembled packet abandoned mid-flight
  volatile uint32_t loraRxCrcErrors   = 0;  // readData() refused it: bad CRC or spurious IRQ
  volatile uint32_t loraRxBadLength   = 0;  // frame shorter than a header or longer than the max
  volatile uint32_t loraRxSpuriousIrq = 0;  // woken with no completed reception to collect
  volatile uint32_t tcpRxPackets  = 0;      // deframed packets from clients
  volatile uint32_t tcpClients    = 0;
};

extern NodeStats g_stats;
