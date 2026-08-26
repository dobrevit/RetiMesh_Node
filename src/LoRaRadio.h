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
#include "Settings.h"

class LoRaRadio {
public:
  // Initializes SPI, detects the transceiver and enters receive mode with
  // the given channel settings. Returns false when no radio answers (the
  // web UI stays up so the node is still debuggable in the field).
  bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, const RadioSettings& s);

  bool online() const { return _online; }
  const char* modelName() const { return _modelName; }
  int8_t maxTxDbm() const { return _sx1262 ? 22 : 17; }

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
  void waitClearChannel();               // simplified CSMA (CAD + backoff)
  bool sendFrame(const uint8_t* frame, size_t len);

  bool probeSX1262(const RadioSettings& s);
  bool probeSX127x(const RadioSettings& s);
  bool applySettings(const RadioSettings& s);   // radio task context only
  void logActive() const;

  static void IRAM_ATTR onRadioIrq();    // just notifies radioTask

  PhysicalLayer* _radio  = nullptr;      // common runtime surface
  SX1262*        _sx1262 = nullptr;      // exactly one of these is set
  SX1276*        _sx1276 = nullptr;
  const char*    _modelName = "none";
  SPIClass       _spi{FSPI};
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

  static TaskHandle_t s_taskHandle;      // notification target for the ISR
};

extern LoRaRadio loraRadio;
