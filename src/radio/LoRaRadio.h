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
//  LoRaRadio.h — LoRa driver (RadioLib) + RNode-compatible RF framing
//
//  The LilyGO T3-S3 ships with either an SX1262 or an SX1276/SX1278, on
//  partly different pins. begin() probes for the SX127x first (DIO0 9)
//  and then for the SX1262 (DIO1 33, BUSY 34), so one build fits every
//  variant. After that everything runs through RadioLib's PhysicalLayer
//  interface — RxDone/TxDone both arrive on the chip's primary IRQ line
//  (DIO1 on SX126x, DIO0 on SX127x), which is what the task waits on.
//
//  Channel parameters come from Settings (NVS). The web UI changes them
//  at runtime via requestReconfigure(); the radio task — sole owner of
//  the chip — applies them between packets, no reboot needed.
//
//  Beacons / neighbour discovery. Two kinds of frame feed the neighbour
//  table:
//    * RetiMesh beacons — a valid Reticulum broadcast to the PLAIN
//      destination "retimesh.beacon" (see RNS_BEACON_* in Config.h) whose
//      payload is "RM1 <T> <name> <version>", T = H hello (boot probe),
//      R reply to a hello (random delay), I periodic id while idle.
//      RNS peers parse and silently drop it (no registered destination),
//      or receive it if they register that plain destination.
//    * RNode station IDs — the raw printable callsign an RNS
//      RNodeInterface transmits (`id_callsign`); RNS itself counts those
//      as malformed packets, we just list them.
//  Beacons are never forwarded to TCP clients; they only feed Neighbors.
//
//  Packet flow, radio side:
//
//    TCP -> LoRa:   radioTask blocks on the TX ring buffer. Each item is
//                   one raw RNS packet (<= 500 bytes). It is fragmented
//                   into RNode-framed LoRa frames and transmitted after
//                   a CSMA clear-channel check.
//
//    LoRa -> TCP:   IRQ fires on RxDone -> ISR notifies radioTask ->
//                   frame is read, reassembled (split packets), and the
//                   complete RNS packet is pushed to the RX ring buffer,
//                   where the bridge task picks it up for TCP broadcast.
//
//  RF framing (byte-compatible with RNode_Firmware, so real RNodes on the
//  same channel parameters can exchange packets with this gateway):
//
//    [ header ] [ payload <= 254 bytes ]
//      header  = (random & 0xF0) | FLAG_SPLIT(0x01 if packet > 254 bytes)
//    Split packets repeat the same header on every fragment; the receiver
//    completes reassembly on the second fragment carrying the same
//    sequence nibble (RNS MTU 500 always fits in two fragments).
// ============================================================================
#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include "Config.h"
#include "Airtime.h"
#include "RadioCaps.h"
#include "Settings.h"

// ---------------------------------------------------------------------------
// The radio's side of the handoff to the transport
// ---------------------------------------------------------------------------
// These were in Config.h, which every translation unit in the firmware
// includes and whose docstring says it holds compile-time configuration
// overridable from build_flags. The layout of a private ring-buffer item and
// an SX127x demodulation-limit formula are neither, and having them there
// compiled the radio's wire format into the settings store, the web server and
// the OTA installer — none of which have any business with it, and any of
// which could come to depend on it. The two files that share this contract are
// this one's .cpp, which fills the ring, and RnsTransport.cpp, which drains
// it; both already include this header.

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


class LoRaRadio {
public:
  // Initializes SPI, detects the transceiver and enters receive mode with
  // the given channel settings. Returns false when no radio answers (the
  // web UI stays up so the node is still debuggable in the field).
  bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, const RadioSettings& s);

  bool online() const { return _online; }
  const char* modelName() const { return _modelName; }
  // What the detected chip can do — frequency span, bandwidth steps, SF and
  // power range, and which regulatory model its band falls under. Callers that
  // used to hardcode sub-GHz limits ask this instead.
  const RadioCaps::Caps& caps() const { return *_caps; }
  // The settable figure is what the chip is driven at, and nothing else: the
  // driver rejects anything above its own maximum, so raising the ceiling for a
  // board with an amplifier only produced settings that failed to apply. What
  // the PA then radiates is a property of the board the operator has to know;
  // hasPa() says one is fitted so the UI and docs can point that out.
  int8_t maxTxDbm() const { return _caps->txMaxDbm; }
  static bool hasPa() { return HAS_PA != 0; }
  const char* callsign() const;          // beacon name: configured, else SSID

  // Hand new channel settings to the radio task. Thread-safe; returns
  // immediately. Result is visible in g_stats.radioApplyError.
  void requestReconfigure(const RadioSettings& s);

  // FreeRTOS entry point — created pinned to core 1 from main.cpp.
  static void radioTask(void* self);

private:
  void taskLoop();
  void handleRadioIrq();                 // RxDone path: read + reassemble
  void deliverPacket(size_t len);        // completed RNS packet -> RX ring
  void transmitPacket(const uint8_t* data, size_t len);
  void csmaWait();                       // DIFS + contention window before TX
  bool mediumFree();                     // one CAD probe
  void refreshAirtimeStats();            // publish channel use into g_stats
  bool sendFrame(const uint8_t* frame, size_t len);

  bool isRetiMeshBeacon(const uint8_t* p, size_t len) const;
  bool isStationId(const uint8_t* p, size_t len) const;
  void handleBeacon(const uint8_t* p, size_t len);
  void sendBeacon(char type);

  bool probeSX1262(const RadioSettings& s);
  bool probeSX127x(const RadioSettings& s);
  bool probeSX1280(const RadioSettings& s);
  bool probeLR1110(const RadioSettings& s);
  void irqSelfTest();                    // proves the IRQ line, see the .cpp
  uint32_t rxDoneFlag() const;           // this chip's RxDone bit, raw
  bool applySettings(const RadioSettings& s);   // radio task context only
  void configureAirtime(const RadioSettings& s);  // symbol time -> duty cycle + CSMA
  void logActive() const;

  static void IRAM_ATTR onRadioIrq();    // just notifies radioTask

  PhysicalLayer* _radio  = nullptr;      // common runtime surface
  SX1262*        _sx1262 = nullptr;      // exactly one of these is set
  SX1276*        _sx1276 = nullptr;
  SX1280*        _sx1280 = nullptr;
  LR1110*        _lr1110 = nullptr;
  const char*    _modelName = "none";
  const RadioCaps::Caps* _caps = &RadioCaps::kUnknown;
  // The host's bus rather than one of this driver's own — the panel and the
  // card may be on the same wires. See sys/SpiBus.h; set in begin().
  SPIClass*      _spi = nullptr;
  RingbufHandle_t _txRing = nullptr;
  RingbufHandle_t _rxRing = nullptr;
  bool           _online = false;

  RadioSettings  _active;                // what the chip is running now
  RadioSettings  _pending;               // handed over by requestReconfigure
  volatile bool  _reconfigure = false;
  portMUX_TYPE   _mux = portMUX_INITIALIZER_UNLOCKED;

  // RX reassembly state (mirrors RNode's seq/read_len logic)
  uint8_t  _frame[LORA_FRAME_MAX + 1];   // one raw LoRa frame
  uint8_t  _rxBuf[2 * LORA_FRAG_PAYLOAD];// reassembled packet (max 508)
  size_t   _rxLen  = 0;
  uint8_t  _rxSeq  = LORA_SEQ_UNSET;

  uint8_t  _txFrame[LORA_FRAME_MAX];

  Airtime  _airtime;                     // time on air, duty cycle, CSMA sizing
  uint32_t _statsAtMs = 0;               // last publish into g_stats

  uint32_t _lastTxMs  = 0;               // any transmission (packet or beacon)
  uint32_t _helloAtMs = 0;               // boot probe due time (0 = done)
  uint32_t _replyAtMs = 0;               // pending reply to someone's hello

  static TaskHandle_t s_taskHandle;      // notification target for the ISR
};

extern LoRaRadio loraRadio;
