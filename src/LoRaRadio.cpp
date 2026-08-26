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
//  LoRaRadio.cpp — see LoRaRadio.h for the framing / flow description.
// ============================================================================
#include "LoRaRadio.h"
#include <esp_random.h>

LoRaRadio loraRadio;
TaskHandle_t LoRaRadio::s_taskHandle = nullptr;

// ---------------------------------------------------------------------------
// ISR: the radio's IRQ line (DIO1 on SX126x, DIO0 on SX127x) rises on
// RxDone / TxDone. Nothing is decided here — the task owns the radio and
// knows (via its own state) which operation finished.
// ---------------------------------------------------------------------------
void IRAM_ATTR LoRaRadio::onRadioIrq() {
  BaseType_t higherPrioWoken = pdFALSE;
  if (s_taskHandle) vTaskNotifyGiveFromISR(s_taskHandle, &higherPrioWoken);
  portYIELD_FROM_ISR(higherPrioWoken);
}

// ---------------------------------------------------------------------------
bool LoRaRadio::begin(RingbufHandle_t txRing, RingbufHandle_t rxRing) {
  _txRing = txRing;
  _rxRing = rxRing;

  _spi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

  // Probe order matters for boot time: the SX127x check is a version-
  // register read that fails within ~100 ms on an SX1262, whereas the
  // SX1262 check waits on BUSY (GPIO 34 = DIO2 on an SX127x board) and
  // needs ~27 s to give up. Both are harmless to the other chip.
  if (!probeSX127x() && !probeSX1262()) {
    log_e("No LoRa transceiver found (tried SX1262 on DIO1=%d/BUSY=%d and "
          "SX127x on DIO0=%d) — check wiring", PIN_LORA_DIO1, PIN_LORA_BUSY,
          PIN_LORA_DIO0);
    return false;
  }

  _radio->setPacketReceivedAction(onRadioIrq);   // DIO1 / DIO0 as appropriate
  _online = true;
  g_stats.radioModel = _modelName;
  log_i("%s online: %.3f MHz, BW %.1f kHz, SF%d, CR 4/%d, %d dBm",
        _modelName, RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_CR, RF_TX_DBM);
  return true;
}

bool LoRaRadio::probeSX1262() {
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, _spi);
  SX1262* sx  = new SX1262(mod);
  int16_t state = sx->begin(RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_CR, RF_SYNCWORD,
                            RF_TX_DBM, RF_PREAMBLE_SYMS, RF_TCXO_VOLTAGE, false);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("SX1262 not found (code %d)", state);
    delete sx; delete mod;
    return false;
  }
  #if RF_DIO2_AS_SWITCH
    sx->setDio2AsRfSwitch(true);
  #endif
  sx->setCurrentLimit(140.0);
  sx->setCRC(true);
  _radio = sx;
  _modelName = "SX1262";
  return true;
}

bool LoRaRadio::probeSX127x() {
  // SX1276 and SX1278 share silicon version 0x12 and this driver; the
  // class only differs in the accepted frequency range, and SX1276 spans
  // both sub-GHz bands.
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1, _spi);
  SX1276* sx  = new SX1276(mod);
  int16_t state = sx->begin(RF_FREQ_MHZ, RF_BW_KHZ, RF_SF, RF_CR, RF_SYNCWORD,
                            RF_TX_DBM, RF_PREAMBLE_SYMS, 0);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("SX127x not found (code %d)", state);
    delete sx; delete mod;
    return false;
  }
  sx->setCurrentLimit(140);
  sx->setCRC(true);
  _radio = sx;
  _modelName = "SX1276";
  _isSX127x = true;
  return true;
}

// ---------------------------------------------------------------------------
// Task body — pinned to CORE 1 by main.cpp. This task is the only code
// that ever touches the SX1262 after begin(), so no radio lock is needed.
// ---------------------------------------------------------------------------
void LoRaRadio::radioTask(void* self) {
  s_taskHandle = xTaskGetCurrentTaskHandle();
  static_cast<LoRaRadio*>(self)->taskLoop();
}

void LoRaRadio::taskLoop() {
  if (!_online) { vTaskDelete(nullptr); return; }

  _radio->startReceive();

  for (;;) {
    // (1) Service the radio: block up to 10 ms for a DIO1 notification.
    //     This doubles as the poll interval for the TX ring below.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0) {
      handleRadioIrq();
    }

    // (2) TCP -> LoRa: pull one complete RNS packet from the ring buffer
    //     (queued there by RetiTransportServer's HDLC deframer) and
    //     transmit it. Non-blocking take; RX keeps priority.
    size_t itemSize = 0;
    uint8_t* item = (uint8_t*)xRingbufferReceive(_txRing, &itemSize, 0);
    if (item != nullptr) {
      transmitPacket(item, itemSize);
      vRingbufferReturnItem(_txRing, item);
    }
  }
}

// ---------------------------------------------------------------------------
// RX path
// ---------------------------------------------------------------------------
void LoRaRadio::handleRadioIrq() {
  // Only RxDone is expected here: TxDone notifications are consumed
  // synchronously inside sendFrame(), and CAD inside waitClearChannel().
  size_t len = _radio->getPacketLength();
  if (len < 1 || len > LORA_FRAME_MAX) { _radio->startReceive(); return; }

  int16_t state = _radio->readData(_frame, len);
  if (state != RADIOLIB_ERR_NONE) {      // CRC error or spurious IRQ
    _radio->startReceive();
    return;
  }

  g_stats.lastRssi = _radio->getRSSI();
  g_stats.lastSnr  = _radio->getSNR();

  // ---- RNode-compatible split-packet reassembly --------------------------
  uint8_t header   = _frame[0];
  uint8_t sequence = header >> 4;
  bool    split    = header & LORA_FLAG_SPLIT;
  const uint8_t* payload = _frame + LORA_HEADER_LEN;
  size_t  payloadLen     = len - LORA_HEADER_LEN;
  bool    ready    = false;

  if (split && _rxSeq == LORA_SEQ_UNSET) {
    // First fragment of a split packet.
    _rxLen = 0;
    _rxSeq = sequence;
    memcpy(_rxBuf, payload, payloadLen);
    _rxLen = payloadLen;
  } else if (split && _rxSeq == sequence) {
    // Second fragment — packet complete (RNS MTU fits in two fragments).
    if (_rxLen + payloadLen <= sizeof(_rxBuf)) {
      memcpy(_rxBuf + _rxLen, payload, payloadLen);
      _rxLen += payloadLen;
      ready = true;
    } else {
      g_stats.loraRxDropped++;
      _rxLen = 0;
    }
    _rxSeq = LORA_SEQ_UNSET;
  } else if (split) {
    // Different sequence — a new split packet started; the old one is lost.
    _rxLen = 0;
    _rxSeq = sequence;
    memcpy(_rxBuf, payload, payloadLen);
    _rxLen = payloadLen;
  } else {
    // Unsplit packet; discard any half-finished reassembly.
    _rxSeq = LORA_SEQ_UNSET;
    memcpy(_rxBuf, payload, payloadLen);
    _rxLen = payloadLen;
    ready  = true;
  }

  if (ready && _rxLen > 0) deliverPacket(_rxLen);

  _radio->startReceive();
}

void LoRaRadio::deliverPacket(size_t len) {
  // LoRa -> TCP handoff: the bridge task (RetiTransportServer::bridgeTask)
  // blocks on this ring, HDLC-frames the packet and broadcasts it to every
  // connected Reticulum client. If all clients are slow and the ring is
  // full we drop — the radio must never stall.
  if (xRingbufferSend(_rxRing, _rxBuf, len, 0) == pdTRUE) {
    g_stats.loraRxPackets++;
  } else {
    g_stats.loraRxDropped++;
  }
  _rxLen = 0;
}

// ---------------------------------------------------------------------------
// TX path
// ---------------------------------------------------------------------------
void LoRaRadio::transmitPacket(const uint8_t* data, size_t len) {
  if (len == 0 || len > sizeof(_rxBuf)) return;

  waitClearChannel();

  // RNode framing: one random sequence nibble for all fragments of this
  // packet, FLAG_SPLIT set when the payload spans more than one frame.
  uint8_t header = (uint8_t)(esp_random() & 0xF0);
  if (len > LORA_FRAG_PAYLOAD) header |= LORA_FLAG_SPLIT;

  size_t offset = 0;
  while (offset < len) {
    size_t chunk = min((size_t)LORA_FRAG_PAYLOAD, len - offset);
    _txFrame[0] = header;
    memcpy(_txFrame + 1, data + offset, chunk);
    if (!sendFrame(_txFrame, chunk + LORA_HEADER_LEN)) break;
    offset += chunk;
  }

  g_stats.loraTxPackets++;
  _radio->startReceive();                // back to listening
}

bool LoRaRadio::sendFrame(const uint8_t* frame, size_t len) {
  ulTaskNotifyTake(pdTRUE, 0);           // flush stale notifications

  int16_t state = _radio->startTransmit((uint8_t*)frame, len);
  if (state != RADIOLIB_ERR_NONE) {
    log_e("startTransmit failed, code %d", state);
    return false;
  }

  // Wait for TxDone via DIO1. SF8/125k worst case is ~450 ms per frame;
  // 5 s means the radio wedged, in which case finishTransmit() cleans up.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
  _radio->finishTransmit();
  return true;
}

// Simplified CSMA: random slotted backoff while listening, then a CAD
// (channel activity detection) probe. Not RNode's full DIFS/contention-
// window machine, but enough to avoid stomping on active transmissions.
void LoRaRadio::waitClearChannel() {
  uint32_t started = millis();

  while (millis() - started < CSMA_MAX_WAIT_MS) {
    // Backoff in RX mode. If a frame lands mid-backoff, service it and
    // restart the backoff — the channel clearly is not free.
    uint32_t slots = 1 + (esp_random() % CSMA_MAX_SLOTS);
    bool sawTraffic = false;
    for (uint32_t i = 0; i < slots; i++) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CSMA_SLOT_MS)) > 0) {
        handleRadioIrq();
        sawTraffic = true;
        break;
      }
    }
    if (sawTraffic) continue;

    // scanChannel() is blocking and drives DIO1 itself; any stray
    // notification it produces is flushed by sendFrame() afterwards.
    int16_t cad = _radio->scanChannel();
    if (cad == RADIOLIB_CHANNEL_FREE) return;

    _radio->startReceive();              // busy — listen and try again
  }
  // Timed out deferring: transmit anyway rather than dropping the packet.
}
