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
#elif defined(BOARD_HELTEC_V3)
  #include "boards/heltec_v3.h"
#elif defined(BOARD_HELTEC_WS)
  #include "boards/heltec_ws.h"
#elif defined(BOARD_HELTEC_WB)
  #include "boards/heltec_wb.h"
#elif defined(BOARD_HELTEC_WP)
  #include "boards/heltec_wp.h"
#elif defined(BOARD_T3S3_SX1280_PA)
  #include "boards/t3s3_sx1280_pa.h"
#elif defined(BOARD_T3S3_SX1280)
  #include "boards/t3s3_sx1280.h"
#else
  #include "boards/t3s3.h"
#endif

// ---------------------------------------------------------------------------
// Local-link capabilities: what the PCB puts on its USB connector. These come
// from boards.json — the one registry of board facts — through
// tools/board_caps.py, which turns the env's "local_link" block into -D flags
// at build time and refuses to build when they contradict the framework's own
// USB flags in platformio.ini. The defaults below are what a build with no
// entry (the host-native test env) gets: nothing.
//
//   BOARD_USB_NATIVE        the MCU's own USB D+/D- reach the connector
//   BOARD_USB_NCM           ...and the silicon can present CDC-NCM there
//   BOARD_USB_BRIDGE        "CP2102", "CH9102", ... or "none"
//   BOARD_BRIDGE_AUTO_RESET the bridge's DTR/RTS reach EN/IO0 (esptool resets it)
//   BOARD_UART_NETWORK      the UART behind the bridge may carry PPP
//   BOARD_UART_INSTANCE     which UART the bridge sits on (0 on every board so far)
//   BOARD_UART_BAUDS        the rates the registry lists for it, comma-separated
//   BOARD_UART_MAX_BAUD     the highest of them tried on this board's hardware
// ---------------------------------------------------------------------------
#ifndef BOARD_USB_NATIVE
  #define BOARD_USB_NATIVE        0
#endif
#ifndef BOARD_USB_NCM
  #define BOARD_USB_NCM           0
#endif
#ifndef BOARD_USB_BRIDGE
  #define BOARD_USB_BRIDGE        "none"
#endif
#ifndef BOARD_BRIDGE_AUTO_RESET
  #define BOARD_BRIDGE_AUTO_RESET 0
#endif
#ifndef BOARD_UART_NETWORK
  #define BOARD_UART_NETWORK      0
#endif
#ifndef BOARD_UART_INSTANCE
  #define BOARD_UART_INSTANCE     0
#endif
#ifndef BOARD_UART_BAUDS
  #define BOARD_UART_BAUDS        115200
#endif
#ifndef BOARD_UART_MAX_BAUD
  #define BOARD_UART_MAX_BAUD     115200
#endif
// Whether this build drives USB networking: the board can present NCM and
// the OTG stack owns the peripheral (ARDUINO_USB_MODE 0, set by
// tools/board_caps.py from the same registry facts). One derived fact, so no
// file has to know both halves. See docs/local-link.md and UsbNcm.h.
#if BOARD_USB_NCM && defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
  #define HAS_USB_NCM 1
#else
  #define HAS_USB_NCM 0
#endif
// Whether this build drives PPP over the bridge UART: every board whose
// registry entry says the UART may carry it does, because the core's
// prebuilt lwIP carries the PPP client on every chip. See PppUart.h.
#if BOARD_UART_NETWORK
  #define HAS_PPP 1
#else
  #define HAS_PPP 0
#endif
// PPP over the bridge UART (PppUart.h). The receive ring is the UART
// driver's own, sized here; what does not fit while the reader is behind is
// dropped and PPP retransmits — the radio never waits for the serial port.
// The transmit queue holds one largest frame (an escaped 1500-byte packet is
// a little over 3 KB) with room to spare; a frame that does not fit is
// dropped whole. The default speed is the console's, so that switching PPP
// on changes nothing a host already relies on; a faster one has to be
// qualified per board (boards.json uart.tested_max_baud).
#ifndef PPP_RX_RING_BYTES
  #define PPP_RX_RING_BYTES   4096
#endif
#ifndef PPP_TX_QUEUE_BYTES
  #define PPP_TX_QUEUE_BYTES  8192          // five frames at PPP's MTU; internal RAM
#endif
#ifndef PPP_BAUD_DEFAULT
  #define PPP_BAUD_DEFAULT    115200
#endif
// There is no smaller pair for the console alone. A board that carries PPP
// carries these sizes from boot whether the switch is on or off, because the
// core fixes them before it installs the UART driver and they cannot be
// resized afterwards (main.cpp, PppUart.h). What the switch gives back is the
// interface and the reader task, not the ring.

// ---------------------------------------------------------------------------
// The maintenance console (Maintenance.h) and its network transport
// (ConsoleServer.h)
//
// The console over a socket is what lets a node be configured from a
// distance without a resident web server: measured on a Heltec Wireless
// Stick, the portal and the resolver beside it cost about 22 KB of
// byte-addressable RAM ("http + dns" in the boot bill, with mDNS billed
// separately at 6 368 B) against 272 B for the Reticulum TCP listener, on a
// board that finishes booting with about 27 KB. Same parser, same settings rules, same refusals
// as the cable — a second transport, not a second implementation.
// ---------------------------------------------------------------------------
#ifndef CONSOLE_TCP_PORT
  #define CONSOLE_TCP_PORT       4243     // beside the Reticulum transport's 4242
#endif
// One caller at a time, and not a setting: the transport holds one client
// slot (ConsoleServer.cpp), so a second would be dead RAM and a macro that
// raised it would be a knob that turns nothing. This is a configuration
// channel rather than a service — two operators changing settings at once is
// a race nobody asked for.
// One for whatever a network transport brings, and one kept back for a caller
// the transport authenticated itself — today that is a command arriving as an
// LXMF message. They are not interchangeable: the TCP console claims its slot
// on connect, before any password, so a single client sitting idle on the
// access point would otherwise deny the one recovery path a node has when its
// cable is dead (RnsAdmin.h, Maintenance.h).
#define MAINT_NET_SESSIONS       2
#define MAINT_RESERVED_SESSIONS  1
// Guessing the admin password over the air. Counted for the node, not the
// connection (Maintenance.h).
#ifndef MAINT_AUTH_MAX_FAILURES
  #define MAINT_AUTH_MAX_FAILURES 3
#endif
#ifndef MAINT_AUTH_LOCKOUT_MS
  #define MAINT_AUTH_LOCKOUT_MS   30000
#endif

// ---------------------------------------------------------------------------
// Firmware version — single-sourced from the git tag by CI
// (PLATFORMIO_BUILD_FLAGS=-DFW_VERSION=\"v1.2.3\"). A build CI did not version
// asks git instead (tools/fw_version.py): `git describe --always --dirty`, so
// a node can say which commit it runs and a bench of them can be told apart.
// "dev" survives only where git cannot answer at all — a source tarball, or a
// container without the binary.
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
// SoftAP WPA3 (SAE) needs ESP-IDF 5, which the core 3.x toolchain brings. The
// gate stays on the IDF version rather than being assumed true because the
// earlier core 2.x (IDF 4.4) rejected the auth mode outright (verified:
// esp_wifi_set_config -> ESP_ERR_INVALID_ARG, AP stays WPA2), and a build on
// such a core must go on saying so rather than offering what it cannot do.
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

// A classic ESP32 splits its internal memory, and much of what the heap
// reports is 32-bit-only IRAM that cannot hold a buffer or a stack. With PSRAM
// there is somewhere else to put things; without it, the byte-addressable half
// is all a node has, and on a Wireless Stick that is about 213 KB against
// 200 KB spent before it has finished starting.
//
// A class rather than a board: today it selects the Wireless Stick alone — the
// Wireless Bridge is the same silicon with 8 MB of PSRAM, and the T-Beam's
// board file brings 4 MB — and it will select the next board like it without
// anybody remembering to add one.
#if defined(CONFIG_IDF_TARGET_ESP32) && !defined(BOARD_HAS_PSRAM)
  #define BOARD_DRAM_TIGHT 1
#else
  #define BOARD_DRAM_TIGHT 0
#endif

// Whether a board starts mDNS by default. It answers <hostname>.local and
// advertises the RNS port: a convenience, not a service anything depends on,
// and 6 368 B of byte-addressable RAM measured with Diag::cost(). On a board
// with ten kilobytes left it is the largest single thing that can be declined
// while still being a node, so the tight ones start without it. It stays a
// setting either way (docs/hardware.md).
#ifndef MDNS_ENABLED_DEFAULT
  #define MDNS_ENABLED_DEFAULT (BOARD_DRAM_TIGHT ? 0 : 1)
#endif

#ifndef MDNS_HOSTNAME
  // Fallback only. The name a node actually answers to is derived from its
  // access-point name — see WifiManager::deriveHostname() — so that two nodes
  // on one network do not both claim the same one. This is what is used when
  // an operator has renamed the AP to something with no usable characters in
  // it at all.
  #define MDNS_HOSTNAME     "retimesh"
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
  // Two on a board with no room to spare: past the cap a client is refused,
  // which an operator sees and which costs the node nothing (see
  // RetiTransportServer). Contrast AUTOIF_MAX_PEERS below, which evicts.
  #define RNS_MAX_CLIENTS   (BOARD_DRAM_TIGHT ? 2 : 4)   // simultaneous Sideband/RNS peers
#endif

// ---------------------------------------------------------------------------
// RNS AutoInterface (IPv6 link-local peering on the access point and the LAN)
// ---------------------------------------------------------------------------
#ifndef HAS_AUTOINTERFACE
  #define HAS_AUTOINTERFACE 1
#endif
#ifndef AUTOIF_GROUP_ID
  #define AUTOIF_GROUP_ID   "reticulum"     // RNS default group id
#endif
// One slot per peer, and a peer is a link-local address rather than a node: a
// neighbour reachable both over this node's access point and over the LAN
// occupies two. Eight was the count of a bench where every node was an AP
// client of the next; a room of them on one LAN needs a slot each, in both
// directions, with the phones and laptops that are the point of the exercise.
#ifndef AUTOIF_MAX_PEERS
  #define AUTOIF_MAX_PEERS  24
#endif

// The most interfaces Transport can hold at once: the radio, one per client on
// :4242, one per AutoInterface peer. Snapshot buffers are sized from this, so
// a listing shows every interface instead of the first few.
#define RNS_MAX_INTERFACES  (1 + RNS_MAX_CLIENTS + AUTOIF_MAX_PEERS)

// An interface name has to hold the longest one this node builds,
// "WiFi/255.255.255.255:65535", with room to spare. RNS derives an
// interface's identity by hashing its name, so a name that gets truncated
// is two interfaces that Transport cannot tell apart.
#define INTERFACE_NAME_MAX  32

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
  // 869.525, not 869.410. The 10 % sub-band is 869.4-869.65, and a channel is
  // not a point: at the default 125 kHz, 869.410 spans 869.3475-869.4725 and
  // so reaches into the unallocated gap below 869.4. The node's own limiter
  // then holds it to 0.1 % and says the channel is not allocated to this kind
  // of device — the firmware was already warning about the default it shipped
  // with. 869.525 sits centred in the 10 % band with room either side.
  #define RF_FREQ_MHZ       869.525         // EU868 SRD, centred in the 10 % sub-band
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

// True where the board puts a power amplifier after the transceiver. It does
// not change what the chip may be driven at — the driver's own maximum still
// applies — but it does change what leaves the antenna, which is the operator's
// to account for. Reported through the API so the settings page can say so.
#ifndef HAS_PA
  #define HAS_PA            0
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
// Display — SSD1306 128x64 on I2C by default (T3-S3: SDA 18 / SCL 17, 0x3C)
// ---------------------------------------------------------------------------
#ifndef HAS_DISPLAY
  #define HAS_DISPLAY       1
#endif

// What kind of glass, which is a different question from how big. The size
// decides the layout; the kind decides which driver is built and what an
// update costs — an e-paper panel of the same dimensions is not a slower
// OLED, it is a panel that has to be told how rarely it may be written.
// A board declares this in its own header; anything that does not say has
// the OLED every board so far has had.
#define DISPLAY_KIND_OLED   1
#define DISPLAY_KIND_EINK   2
#ifndef DISPLAY_KIND
  #define DISPLAY_KIND      DISPLAY_KIND_OLED
#endif
#ifndef PIN_OLED_SDA
  #define PIN_OLED_SDA      18
#endif
#ifndef PIN_OLED_SCL
  #define PIN_OLED_SCL      17
#endif
// Panels on a switched rail need it brought up before they are probed, and it
// is active low on every board that has one so far.
#ifndef HAS_DISPLAY_VEXT
  #define HAS_DISPLAY_VEXT  0
#endif
#ifndef PIN_OLED_RST
  #define PIN_OLED_RST      -1
#endif

#ifndef OLED_ADDR
  #define OLED_ADDR         0x3C
#endif
#ifndef OLED_ROTATION
  #define OLED_ROTATION     0               // 0..3, quarter turns
#endif
// Panel geometry. A board with a different panel says so here and everything
// downstream follows from it — see DisplayLayout.h, which turns these into the
// text grid, the chrome and the refresh interval each page is drawn against.
// An SSD1306 reports nothing about its own size, so the board is what knows.
#ifndef DISPLAY_WIDTH
  #define DISPLAY_WIDTH     128
#endif
#ifndef DISPLAY_HEIGHT
  #define DISPLAY_HEIGHT    64
#endif
// Panels too small for the full page set. A macro as well as the constexpr in
// DisplayLayout.h because the page enum itself has to change, not only what is
// drawn — a page that cannot work on this panel should not be in the cycle at
// all, costing a button press to reach and another to leave.
#if DISPLAY_WIDTH < 96
  #define DISPLAY_COMPACT   1
#else
  #define DISPLAY_COMPACT   0
#endif
// BOOT button (GPIO 0, active low) doubles as the display navigation key:
// short press = next page, long press = blank/wake the panel.
#ifndef PIN_BUTTON
  #define PIN_BUTTON        0
#endif
// Boards that wire an indicator LED name it here. Nothing drives it yet; it is
// claimed at boot and held off so the pin is not left floating.
// LEDs, where the board has them (Leds.h): -1 for none. Boards whose LEDs
// light on LOW say so with LED_ACTIVE_HIGH 0.
#ifndef PIN_STATUS_LED
  #define PIN_STATUS_LED    -1
#endif
#ifndef PIN_WIFI_LED
  #define PIN_WIFI_LED      -1
#endif
#ifndef PIN_LORA_LED
  #define PIN_LORA_LED      -1
#endif
#ifndef PIN_BLE_LED
  #define PIN_BLE_LED       -1               // a lamp labelled for Bluetooth: claimed, kept dark
#endif
#ifndef LED_ACTIVE_HIGH
  #define LED_ACTIVE_HIGH   1
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
// A neighbour that has just appeared knows nothing about this node, and the
// periodic announce can be a whole announce_interval away — ten minutes by
// default, which reads as a dead node. Announce shortly after one registers,
// with enough of a gap that a handful arriving together share one announce.
#define ANNOUNCE_ON_PEER_DELAY_MS 2000
#define ANNOUNCE_MAX_LEN    256
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
//
// Storage goes to PSRAM where the board has any and to internal RAM where it
// does not, which makes the size two different questions rather than one: 24 KB
// is nothing out of two megabytes of PSRAM, and it is more than a tenth of
// everything a Heltec Wireless Stick has. Measured, once the boot log started
// billing each subsystem (Diag.h): 26916 B on the Stick, 26124 B on a Heltec
// V3, on boards that finish booting with 4.5 KB and 51 KB respectively.
//
// Each ring holds whole RNS packets — NOSPLIT, RNS_MTU 500 plus framing — so
// 8192 B is about sixteen packets and 4096 B about eight. Eight is seconds of
// backlog at LoRa speeds, by which time a packet is stale and the sender has
// given up on it anyway; the value is here rather than inline so a board that
// wants another can say so in platformio.ini. A ring that turns out to be too
// small says so instead of hiding it: the send fails, "LoRa TX ring full" is
// logged, and lora_rx_drop_ring counts it in /api/status.
// ---------------------------------------------------------------------------
#ifndef RING_BYTES
  #ifdef BOARD_HAS_PSRAM
    #define RING_BYTES      8192
  #else
    #define RING_BYTES      4096
  #endif
#endif
#define TX_RING_BYTES       RING_BYTES        // TCP  -> LoRa direction
#define RX_RING_BYTES       RING_BYTES        // LoRa -> TCP  direction
#define TCP_IN_RING_BYTES   RING_BYTES        // TCP clients + AutoInterface -> Transport

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

// Restart into the application or the bootloader: the delay before the
// quiesce step, chosen so the HTTP reply or console acknowledgement has left
// the network stack. A settings save that follows with a restart uses the
// longer figure the settings page always waited.
#define RESTART_ACK_DELAY_MS      600
#define RESTART_SETTINGS_DELAY_MS 1500

extern NodeStats g_stats;

// One frame on its way from the radio to the transport: what the radio
// measured of it, then its bytes.
//
// The reading travels with the frame because it is a fact about *that* frame.
// It used to be sampled from g_stats when the transport got round to draining
// the ring, so a backlog of three handed all three the newest one's RSSI —
// and the number a client asks for in a signal report is exactly this one.
struct LoRaRxFrame {
  float rssi;
  float snr;
};

// Link quality from SNR, against a floor that moves with the spreading factor:
// SF12 decodes far below the noise where SF7 cannot, so the same SNR means
// something different on each. The floor is the SX127x demodulation limit at
// each factor — -7.5 dB at SF7 down to -20 dB at SF12, i.e. 10 - 2.5*SF.
// Below it the chip cannot decode at all, so a dead link reads empty rather
// than showing four of seven bars.
//
// Here rather than in the display, because it is now two readers' answer to
// one question: the panel's signal meter and the quality figure a client is
// told in a signal report. RNS derives its own the same way (RNodeInterface).
inline uint8_t loraQualityPercent(float snr, uint8_t sf) {
  const float lo = 10.0f - 2.5f * (float)sf, hi = 6.0f;
  if (snr <= lo) return 0;
  if (snr >= hi) return 100;
  return (uint8_t)((snr - lo) * 100.0f / (hi - lo));
}
