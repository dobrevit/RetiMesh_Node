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
#include "Neighbors.h"
#include "WifiManager.h"

LoRaRadio loraRadio;
TaskHandle_t LoRaRadio::s_taskHandle = nullptr;

static int8_t clampPower(int8_t dbm, int8_t maxDbm) {
  if (dbm > maxDbm) return maxDbm;
  if (dbm < 2) return 2;
  return dbm;
}

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
bool LoRaRadio::begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, const RadioSettings& s) {
  _txRing = txRing;
  _rxRing = rxRing;
  _active = s;

  _spi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

  // Probe order matters for boot time: the SX127x check is a version-
  // register read that fails within ~100 ms on an SX1262, whereas the
  // SX1262 check waits on BUSY (GPIO 34 = DIO2 on an SX127x board) and
  // needs ~27 s to give up. Both are harmless to the other chip.
  if (!probeSX127x(s) && !probeSX1262(s)) {
    log_e("No LoRa transceiver found (tried SX127x on DIO0=%d and SX1262 on "
          "DIO1=%d/BUSY=%d) — check wiring", PIN_LORA_DIO0, PIN_LORA_DIO1, PIN_LORA_BUSY);
    return false;
  }

  _radio->setPacketReceivedAction(onRadioIrq);   // DIO1 / DIO0 as appropriate
  _online = true;
  g_stats.radioModel = _modelName;
  configureAirtime(s);                           // the probes bypass applySettings()
  logActive();
  return true;
}

// Airtime maths follows the channel: symbol time drives both the duty-cycle
// accounting and the CSMA slot length. RadioLib leaves CRC and the explicit
// header on, which is what RNode-compatible framing expects.
void LoRaRadio::configureAirtime(const RadioSettings& s) {
  Airtime::Params ap;
  ap.sf = s.sf; ap.bwKhz = s.bwKhz; ap.cr = s.cr;
  ap.preambleSyms = s.preamble; ap.crcOn = true; ap.implicitHeader = false;
  _airtime.configure(ap);
  g_stats.csmaSlotMs = (uint16_t)_airtime.slotMs();

  // The transmit budget belongs to the sub-band, so it is re-derived whenever
  // the channel moves.
  const Airtime::Band* band = Airtime::bandFor(s.freqMhz, s.bwKhz);
  const uint16_t limit = Airtime::effectiveBasisPoints(s.freqMhz, s.bwKhz, s.dutyCyclePct);
  g_stats.dutyLimitBp = limit;
  if (band && !band->allocated) {
    log_w("channel overlaps EU SRD %s — this range is not allocated to this kind of device; "
          "holding to %.2f %% of the hour", band->name, limit / 100.0f);
    // Landing here is usually a near miss rather than a deliberate choice: the
    // centre sits in a generous sub-band but the channel's skirt reaches into
    // the gap beside it. Say what centre would fit, so the fix is obvious.
    const Airtime::Band* best = Airtime::mostGenerousOverlapping(s.freqMhz, s.bwKhz);
    if (best && best->basisPoints > band->basisPoints)
      log_w("  a %.0f kHz channel needs its centre at %.4f MHz or above to sit inside %s "
            "(%.1f %%); at %.4f MHz it reaches below %.3f MHz",
            (double)s.bwKhz, (double)(best->lowMhz + s.bwKhz / 2000.0f), best->name,
            best->basisPoints / 100.0f, (double)s.freqMhz, (double)best->lowMhz);
  }
  else if (band)
    log_i("channel is in EU SRD %s — holding to %.2f %% of the hour", band->name, limit / 100.0f);
  else if (limit) log_w("%.3f MHz is outside the EU 863-870 plan: applying the configured %u %% limit, "
                        "check your local rules", (double)s.freqMhz, (unsigned)s.dutyCyclePct);
  else log_w("%.3f MHz is outside the EU 863-870 plan and no duty cycle is set — transmitting unlimited, "
             "which is unlikely to be legal anywhere", (double)s.freqMhz);
}

bool LoRaRadio::probeSX1262(const RadioSettings& s) {
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, _spi);
  SX1262* sx  = new SX1262(mod);
  int16_t state = sx->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, 22), s.preamble, RF_TCXO_VOLTAGE, false);
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
  _radio = _sx1262 = sx;
  _modelName = "SX1262";
  return true;
}

bool LoRaRadio::probeSX127x(const RadioSettings& s) {
  // SX1276 and SX1278 share silicon version 0x12 and this driver; the
  // class only differs in the accepted frequency range, and SX1276 spans
  // both sub-GHz bands.
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1, _spi);
  SX1276* sx  = new SX1276(mod);
  int16_t state = sx->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, 17), s.preamble, 0);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("SX127x not found (code %d)", state);
    delete sx; delete mod;
    return false;
  }
  sx->setCurrentLimit(140);
  sx->setCRC(true);
  _radio = _sx1276 = sx;
  _modelName = "SX1276";
  return true;
}

void LoRaRadio::logActive() const {
  log_i("%s online: %.3f MHz, BW %.1f kHz, SF%d, CR 4/%d, %d dBm, sync 0x%02X, preamble %u",
        _modelName, _active.freqMhz, _active.bwKhz, _active.sf, _active.cr,
        clampPower(_active.txDbm, maxTxDbm()), _active.syncWord, _active.preamble);
}

// ---------------------------------------------------------------------------
// Runtime reconfiguration
// ---------------------------------------------------------------------------
void LoRaRadio::requestReconfigure(const RadioSettings& s) {
  portENTER_CRITICAL(&_mux);
  _pending = s;
  _reconfigure = true;
  portEXIT_CRITICAL(&_mux);
}

// Called from the radio task only. Leaves the chip in standby; the caller
// re-arms receive. On failure the previous settings are restored.
bool LoRaRadio::applySettings(const RadioSettings& s) {
  // The LoRa modulation setters are not part of PhysicalLayer, so they go
  // through whichever concrete driver was detected.
  #define CHIP(call) (_sx1262 ? _sx1262->call : _sx1276->call)
  struct Step { const char* what; int16_t code; };
  Step steps[] = {
    { "standby",          _radio->standby() },
    { "frequency",        CHIP(setFrequency(s.freqMhz)) },
    { "bandwidth",        CHIP(setBandwidth(s.bwKhz)) },
    { "spreading factor", CHIP(setSpreadingFactor(s.sf)) },
    { "coding rate",      CHIP(setCodingRate(s.cr)) },
    { "tx power",         CHIP(setOutputPower(clampPower(s.txDbm, maxTxDbm()))) },
    { "preamble",         CHIP(setPreambleLength(s.preamble)) },
    { "sync word",        CHIP(setSyncWord(s.syncWord)) },
  };
  #undef CHIP
  for (const Step& st : steps) {
    if (st.code != RADIOLIB_ERR_NONE) {
      log_e("radio reconfigure failed at %s (code %d)", st.what, st.code);
      g_stats.radioApplyError = st.code;
      return false;
    }
  }

  configureAirtime(s);
  return true;
}

// ---------------------------------------------------------------------------
// Task body — pinned to CORE 1 by main.cpp. This task is the only code
// that ever touches the transceiver after begin(), so no radio lock is
// needed.
// ---------------------------------------------------------------------------
void LoRaRadio::radioTask(void* self) {
  s_taskHandle = xTaskGetCurrentTaskHandle();
  static_cast<LoRaRadio*>(self)->taskLoop();
}

void LoRaRadio::taskLoop() {
  if (!_online) { vTaskDelete(nullptr); return; }

  _radio->startReceive();
  _lastTxMs  = millis();
  _helloAtMs = millis() + BEACON_HELLO_DELAY_MS;

  for (;;) {
    // (0) Settings changed from the web UI? Apply between packets.
    if (_reconfigure) {
      RadioSettings s;
      portENTER_CRITICAL(&_mux);
      s = _pending;
      _reconfigure = false;
      portEXIT_CRITICAL(&_mux);

      if (applySettings(s)) {
        _active = s;
        g_stats.radioApplyError = 0;
        logActive();
      } else {
        applySettings(_active);              // roll back to what worked
      }
      _rxSeq = LORA_SEQ_UNSET; _rxLen = 0;   // half packets are meaningless now
      _radio->startReceive();
    }

    // (1) Service the radio: block up to 10 ms for an IRQ notification.
    //     This doubles as the poll interval for the TX ring below.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)) > 0) {
      handleRadioIrq();
    }

    // (1a) Channel-use figures, and the duty-cycle verdict they feed.
    refreshAirtimeStats();

    // (1b) Beacons: boot hello, pending reply, periodic id when idle.
    //      At most one beacon per loop pass, and the idle check reads the
    //      clock fresh — a transmission above would otherwise make the
    //      stale `now` minus _lastTxMs wrap and fire immediately.
    if (_active.beaconInterval > 0 && !g_stats.dutyLocked) {
      uint32_t now = millis();
      if (_helloAtMs && (int32_t)(now - _helloAtMs) >= 0)      { _helloAtMs = 0; sendBeacon('H'); }
      else if (_replyAtMs && (int32_t)(now - _replyAtMs) >= 0) { _replyAtMs = 0; sendBeacon('R'); }
      else if ((int32_t)(now - _lastTxMs) >= (int32_t)_active.beaconInterval * 1000) sendBeacon('I');
    }

    // (2) TCP -> LoRa: pull one complete RNS packet from the ring buffer
    //     (queued there by RetiTransportServer's HDLC deframer) and
    //     transmit it. Non-blocking take; RX keeps priority.
    //     While the hourly transmit budget is spent, leave packets in the
    //     ring: they go out when the window slides rather than being dropped,
    //     and the sender sees back-pressure instead of silence.
    if (!g_stats.dutyLocked) {
      size_t itemSize = 0;
      uint8_t* item = (uint8_t*)xRingbufferReceive(_txRing, &itemSize, 0);
      if (item != nullptr) {
        transmitPacket(item, itemSize);
        vRingbufferReturnItem(_txRing, item);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// RX path
// ---------------------------------------------------------------------------
void LoRaRadio::handleRadioIrq() {
  // Only RxDone is expected here: TxDone notifications are consumed
  // synchronously inside sendFrame(), and CAD inside csmaWait().
  size_t len = _radio->getPacketLength();
  // At least one payload byte behind the header. A header-only frame used to
  // pass this guard, yield a zero-length payload, and then fall out of the
  // reassembly below without touching any counter — a frame that simply
  // vanished. Expressed against LORA_HEADER_LEN so it stays right if the
  // framing ever grows.
  if (len <= LORA_HEADER_LEN || len > LORA_FRAME_MAX) {
    g_stats.loraRxBadLength++;
    _radio->startReceive();
    return;
  }

  int16_t state = _radio->readData(_frame, len);
  if (state != RADIOLIB_ERR_NONE) {      // CRC error or spurious IRQ
    // Counted, because a node hearing hundreds of these an hour is sitting in
    // interference — which looks nothing like a node whose consumer is slow,
    // and used to be indistinguishable from it.
    g_stats.loraRxCrcErrors++;
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
      g_stats.loraRxDropReasm++;
      _rxLen = 0;
    }
    _rxSeq = LORA_SEQ_UNSET;
  } else if (split) {
    // Different sequence — a new split packet started; the old one is lost.
    // That loss was previously silent, which is the worst kind: two senders
    // interleaving fragments would quietly destroy each other's packets and
    // nothing in the stats would say so.
    if (_rxLen > 0) g_stats.loraRxDropPartial++;
    _rxLen = 0;
    _rxSeq = sequence;
    memcpy(_rxBuf, payload, payloadLen);
    _rxLen = payloadLen;
  } else {
    // Unsplit packet; discard any half-finished reassembly. This is the same
    // loss as the mismatched-sequence case above and has to be counted the
    // same way: under interleaved traffic it is arguably the commoner of the
    // two, since any ordinary packet arriving between two fragments does it.
    if (_rxSeq != LORA_SEQ_UNSET && _rxLen > 0) g_stats.loraRxDropPartial++;
    _rxSeq = LORA_SEQ_UNSET;
    memcpy(_rxBuf, payload, payloadLen);
    _rxLen = payloadLen;
    ready  = true;
  }

  if (ready && _rxLen > 0) {
    if (isRetiMeshBeacon(_rxBuf, _rxLen))  { handleBeacon(_rxBuf + RNS_BEACON_HDR_LEN, _rxLen - RNS_BEACON_HDR_LEN); _rxLen = 0; }
    else if (isStationId(_rxBuf, _rxLen))  { handleBeacon(_rxBuf, _rxLen); _rxLen = 0; }
    else                                   deliverPacket(_rxLen);
  }

  _radio->startReceive();
}

// ---------------------------------------------------------------------------
// Beacons
// ---------------------------------------------------------------------------
const char* LoRaRadio::callsign() const {
  return _active.callsign[0] ? _active.callsign : wifiManager.ssid();
}

static const uint8_t kBeaconDest[16] = RNS_BEACON_DEST_HASH;

static bool printableAscii(const uint8_t* p, size_t len) {
  for (size_t i = 0; i < len; i++) if (p[i] < 0x20 || p[i] > 0x7E) return false;
  return true;
}

// Reticulum broadcast to the retimesh.beacon PLAIN destination, 0 hops,
// carrying printable text.
bool LoRaRadio::isRetiMeshBeacon(const uint8_t* p, size_t len) const {
  if (len <= RNS_BEACON_HDR_LEN || len > RNS_BEACON_HDR_LEN + BEACON_MAX_LEN) return false;
  if (p[0] != RNS_BEACON_FLAGS || p[1] != 0 || p[18] != 0) return false;
  if (memcmp(p + 2, kBeaconDest, sizeof(kBeaconDest)) != 0) return false;
  return printableAscii(p + RNS_BEACON_HDR_LEN, len - RNS_BEACON_HDR_LEN);
}

// Printable ASCII only and short: an RNode station ID. A real RNS packet
// is >= 19 bytes with a 16-byte random hash inside, so the chance of one
// passing this test is ~(95/256)^16 — negligible.
bool LoRaRadio::isStationId(const uint8_t* p, size_t len) const {
  return len > 0 && len <= BEACON_MAX_LEN && printableAscii(p, len);
}

void LoRaRadio::handleBeacon(const uint8_t* p, size_t len) {
  char text[BEACON_MAX_LEN + 1];
  memcpy(text, p, len); text[len] = '\0';
  g_stats.beaconsRx++;

  if (strncmp(text, "RM1 ", 4) == 0 && len >= 6) {
    char type = text[4];
    char name[33] = {0}, ver[16] = {0};
    sscanf(text + 5, " %32s %15s", name, ver);
    if (name[0] == '\0') return;
    if (strcmp(name, callsign()) == 0) return;      // our own echo
    Neighbor n = {};
    strlcpy(n.name, name, sizeof(n.name)); strlcpy(n.version, ver, sizeof(n.version));
    n.kind = NeighborKind::Beacon; n.rssi = g_stats.lastRssi; n.snr = g_stats.lastSnr;
    neighbors.seen(n);
    log_i("beacon %c from %s %s (%.0f dBm / %.1f dB)", type, name, ver, g_stats.lastRssi, g_stats.lastSnr);
    // Answer a hello after a random delay so several neighbours don't collide.
    if (type == 'H' && _active.beaconInterval > 0 && _replyAtMs == 0)
      _replyAtMs = millis() + 300 + (esp_random() % 1700);
  } else {
    Neighbor n = {};
    strlcpy(n.name, text, sizeof(n.name));
    n.kind = NeighborKind::StationId; n.rssi = g_stats.lastRssi; n.snr = g_stats.lastSnr;
    neighbors.seen(n);
    log_i("station id \"%s\" (%.0f dBm / %.1f dB)", n.name, g_stats.lastRssi, g_stats.lastSnr);
  }
}

void LoRaRadio::sendBeacon(char type) {
  uint8_t frame[RNS_BEACON_HDR_LEN + BEACON_MAX_LEN];
  frame[0] = RNS_BEACON_FLAGS;
  frame[1] = 0;                          // hops
  memcpy(frame + 2, kBeaconDest, sizeof(kBeaconDest));
  frame[18] = 0;                         // context: none
  char* text = (char*)frame + RNS_BEACON_HDR_LEN;
  int n = snprintf(text, BEACON_MAX_LEN + 1, "RM1 %c %s %s", type, callsign(), FW_VERSION);
  if (n <= 0) return;
  if ((size_t)n > BEACON_MAX_LEN) n = BEACON_MAX_LEN;
  transmitPacket(frame, RNS_BEACON_HDR_LEN + (size_t)n);
  g_stats.loraTxPackets--;               // transmitPacket counted it as data
  g_stats.beaconsTx++;
  log_i("beacon %c sent: \"%.*s\"", type, n, text);
}

void LoRaRadio::deliverPacket(size_t len) {
  // LoRa -> Reticulum handoff: LoRaRnsInterface::loop() on the RNS task drains
  // this ring and feeds Transport::inbound. If that task is busy — a long pass
  // through reticulum.loop(), or store I/O — the ring fills and we drop rather
  // than stall the radio.
  if (xRingbufferSend(_rxRing, _rxBuf, len, 0) == pdTRUE) {
    g_stats.loraRxPackets++;
  } else {
    g_stats.loraRxDropRing++;
  }
  _rxLen = 0;
}

// ---------------------------------------------------------------------------
// TX path
// ---------------------------------------------------------------------------
void LoRaRadio::transmitPacket(const uint8_t* data, size_t len) {
  if (len == 0 || len > sizeof(_rxBuf)) return;

  csmaWait();

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
  _lastTxMs = millis();
  _radio->startReceive();                // back to listening
}

bool LoRaRadio::sendFrame(const uint8_t* frame, size_t len) {
  ulTaskNotifyTake(pdTRUE, 0);           // flush stale notifications

  int16_t state = _radio->startTransmit((uint8_t*)frame, len);
  if (state != RADIOLIB_ERR_NONE) {
    log_e("startTransmit failed, code %d", state);
    return false;
  }

  // Wait for TxDone. SF12/125k worst case is ~5 s per frame; 8 s means the
  // radio wedged, in which case finishTransmit() cleans up.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8000));
  _radio->finishTransmit();
  _airtime.addTx(millis(), _airtime.timeOnAirMs(len));
  return true;
}

// One channel-activity-detection probe. scanChannel() blocks for roughly a
// symbol and drives the IRQ line itself; sendFrame() flushes any stray
// notification it leaves behind.
bool LoRaRadio::mediumFree() {
  int16_t cad = _radio->scanChannel();
  if (cad == RADIOLIB_CHANNEL_FREE) return true;
  _radio->startReceive();                // busy — go back to listening
  return false;
}

// CSMA as RNode does it: wait for the medium to be free, hold it free for a
// DIFS, then count down a randomly chosen contention window. Any traffic
// during either wait restarts the whole thing, so a node that has just heard
// a packet defers to whoever is mid-exchange. The window is drawn from a band
// selected by recent channel use, which spreads nodes out as the channel
// fills instead of having them all pile in after the same fixed backoff.
void LoRaRadio::csmaWait() {
  const uint32_t slot = _airtime.slotMs();
  const uint32_t difs = _airtime.difsMs();
  uint8_t cwMin = 0, cwMax = Airtime::CW_PER_BAND - 1;
  const float shortTerm = _airtime.shortTermUtil(millis());
  _airtime.contentionWindow(shortTerm, cwMin, cwMax);

  const uint32_t target = (uint32_t)(cwMin + (esp_random() % (uint32_t)(cwMax - cwMin + 1))) * slot;
  const uint32_t started = millis();
  uint32_t waited = 0;                   // contention time accumulated so far

  while (millis() - started < CSMA_MAX_WAIT_MS) {
    if (!mediumFree()) {                 // someone is transmitting: start over
      waited = 0;
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CSMA_CAD_RETRY_MS)) > 0) handleRadioIrq();
      continue;
    }

    // DIFS: the channel must stay quiet for two slots before we even start
    // counting down. A frame arriving here means it was not really idle.
    const uint32_t difsStart = millis();
    bool disturbed = false;
    while (millis() - difsStart < difs) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slot)) > 0) { handleRadioIrq(); disturbed = true; break; }
    }
    if (disturbed) { waited = 0; continue; }

    // Contention window, one slot at a time so an incoming frame can pause it.
    while (waited < target) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slot)) > 0) { handleRadioIrq(); disturbed = true; break; }
      waited += slot;
      if (millis() - started >= CSMA_MAX_WAIT_MS) break;
    }
    if (disturbed) { waited = 0; continue; }
    return;                              // channel held quiet: transmit
  }
  // Deferred for the whole window without a clear run. Transmit anyway rather
  // than dropping the packet — the queue would only grow behind it.
  log_d("CSMA gave up deferring after %u ms", (unsigned)(millis() - started));
}

// Publishes channel use for the web UI and display. Cheap, but there is no
// point recomputing it more than once a second.
void LoRaRadio::refreshAirtimeStats() {
  const uint32_t now = millis();
  if (now - _statsAtMs < 1000) return;
  _statsAtMs = now;
  g_stats.airtimeShort = _airtime.shortTermUtil(now);
  g_stats.csmaBand     = _airtime.cwBand(g_stats.airtimeShort);
  g_stats.airtimeLong  = _airtime.longTermUtil(now);
  const uint16_t limit = Airtime::effectiveBasisPoints(_active.freqMhz, _active.bwKhz, _active.dutyCyclePct);
  g_stats.dutyLimitBp = limit;
  g_stats.dutyBudget   = _airtime.budgetUsed(now, limit);
  const bool locked    = _airtime.locked(now, limit);
  if (locked != g_stats.dutyLocked)
    log_w("duty cycle %s: %.2f %% of the hour used, limit %.2f %%",
          locked ? "reached, holding transmissions" : "back under the limit",
          g_stats.airtimeLong * 100.0f, limit / 100.0f);
  g_stats.dutyLocked = locked;
  g_stats.dutyRetryS = _airtime.retryAfterS(now, limit);
}
