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
//  LoRaRadio.h — SX1262 driver (RadioLib) + RNode-compatible RF framing
//
//  Packet flow, radio side:
//
//    TCP -> LoRa:   radioTask blocks on the TX ring buffer. Each item is
//                   one raw RNS packet (<= 500 bytes). It is fragmented
//                   into RNode-framed LoRa frames and transmitted after
//                   a CSMA clear-channel check.
//
//    LoRa -> TCP:   DIO1 fires on RxDone -> ISR notifies radioTask ->
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

class LoRaRadio {
public:
  // Initializes SPI + SX1262 and enters receive mode.
  // Returns false when the radio is not responding (web UI stays up so
  // the node is still debuggable in the field).
  bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing);

  bool online() const { return _online; }

  // FreeRTOS entry point — created pinned to core 1 from main.cpp.
  static void radioTask(void* self);

private:
  void taskLoop();
  void handleRadioIrq();                 // RxDone path: read + reassemble
  void deliverPacket(size_t len);        // completed RNS packet -> RX ring
  void transmitPacket(const uint8_t* data, size_t len);
  void waitClearChannel();               // simplified CSMA (CAD + backoff)
  bool sendFrame(const uint8_t* frame, size_t len);

  static void IRAM_ATTR onDio1();        // just notifies radioTask

  SX1262*      _radio  = nullptr;
  SPIClass     _spi{FSPI};
  RingbufHandle_t _txRing = nullptr;
  RingbufHandle_t _rxRing = nullptr;
  bool         _online = false;

  // RX reassembly state (mirrors RNode's seq/read_len logic)
  uint8_t  _frame[LORA_FRAME_MAX + 1];   // one raw LoRa frame
  uint8_t  _rxBuf[2 * LORA_FRAG_PAYLOAD];// reassembled packet (max 508)
  size_t   _rxLen  = 0;
  uint8_t  _rxSeq  = LORA_SEQ_UNSET;

  uint8_t  _txFrame[LORA_FRAME_MAX];

  static TaskHandle_t s_taskHandle;      // notification target for the ISR
};

extern LoRaRadio loraRadio;
