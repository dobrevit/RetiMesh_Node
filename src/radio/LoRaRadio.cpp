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
#include "Diag.h"
#include "LoRaRadio.h"
#include "LoRaFem.h"
#include <esp_random.h>
#include "Neighbors.h"
#include "WifiManager.h"
#include "Watchdog.h"

LoRaRadio loraRadio;
TaskHandle_t LoRaRadio::s_taskHandle = nullptr;

// Both bounds come from the caller. The floor used to be a hardcoded 2 dBm,
// which quietly rewrote anything lower — so an SX1280 asked for -10 dBm, a
// figure the API accepted and kept reporting, transmitted at 2 dBm instead.
static int8_t clampPower(int8_t dbm, int8_t minDbm, int8_t maxDbm) {
  if (dbm > maxDbm) return maxDbm;
  if (dbm < minDbm) return minDbm;
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

#if RF_MODEM_SX1280
  // A board reflashed from a sub-GHz image still holds that channel in NVS, and
  // none of it is usable here: the frequency is out of range and the bandwidth
  // is from a list this chip does not share a single value with. Correct the
  // whole channel once, before the probe, so begin(), _active, the airtime
  // budget and the status API all describe the same radio.
  if (!RadioCaps::channelUsable(RadioCaps::kSX1280, _active.freqMhz, _active.bwKhz, _active.sf)) {
    log_w("stored channel %.3f MHz / %.1f kHz / SF%u cannot be tuned by an SX1280 — starting on "
          "%.3f MHz / %.1f kHz / SF%u instead; set a 2.4 GHz channel in the settings",
          (double)_active.freqMhz, (double)_active.bwKhz, (unsigned)_active.sf,
          (double)RF_FREQ_MHZ, (double)RF_BW_KHZ, (unsigned)RF_SF);
    _active.freqMhz = RF_FREQ_MHZ;
    _active.bwKhz   = RF_BW_KHZ;
    _active.sf      = RF_SF;
  }
  // The 2.4 GHz part is selected at build time rather than probed. Detection
  // works by tuning the chip and seeing whether it answers, and an SX1280 will
  // not accept an 868 MHz channel any more than an SX1262 will accept 2.4 GHz:
  // whichever settings the probe carries, one of the two is guaranteed to fail
  // for the wrong reason. A board either has this radio or it does not, and the
  // board header says which.
  if (!probeSX1280(_active)) {
    log_e("No SX1280 found on DIO1=%d/BUSY=%d — check wiring", PIN_LORA_DIO1, PIN_LORA_BUSY);
    return false;
  }
#else
  // Probe order matters for boot time: the SX127x check is a version-
  // register read that fails within ~100 ms on an SX1262, whereas the
  // SX1262 check waits on BUSY (GPIO 34 = DIO2 on an SX127x board) and
  // needs ~27 s to give up. Both are harmless to the other chip.
  if (!probeSX127x(s) && !probeSX1262(s)) {
    log_e("No LoRa transceiver found (tried SX127x on DIO0=%d and SX1262 on "
          "DIO1=%d/BUSY=%d) — check wiring", PIN_LORA_DIO0, PIN_LORA_DIO1, PIN_LORA_BUSY);
    return false;
  }
#endif

  _radio->setPacketReceivedAction(onRadioIrq);   // DIO1 / DIO0 as appropriate
  _online = true;
  g_stats.radioModel = _modelName;
  configureAirtime(_active);                     // the probes bypass applySettings()
  logActive();
  #if RADIO_SELFTEST_ON_BOOT
    irqSelfTest();
  #endif
  return true;
}

// Does the interrupt line actually reach us?
//
// Nothing in an ordinary boot answers that. The transceiver is reached over
// SPI, so it answers begin(), reports its state and prints itself online
// whatever pin the IRQ is on — and a board whose IRQ pin was guessed wrong
// looks identical to one wired correctly right up until the first packet
// fails to arrive. That is not a theoretical worry: this driver shipped with
// the SX1262's pins on an SX1280 board and the boot log was byte-identical.
//
// A transmission settles it. TxDone raises the same line RxDone does, and
// sendFrame() waits on it with an 8 s timeout, so a working line answers in
// tens of milliseconds and a wrong one takes the full timeout. One short
// frame, once, and the log says which.
void LoRaRadio::irqSelfTest() {
  // The ISR notifies s_taskHandle, and radioTask has not started yet — begin()
  // runs from setup(). Without this the interrupt fires into a null handle and
  // the test reports a dead line on perfectly good wiring, which is precisely
  // the false negative it exists to rule out.
  TaskHandle_t previous = s_taskHandle;
  s_taskHandle = xTaskGetCurrentTaskHandle();

  // Non-printable payload on purpose: isStationId() treats any short printable
  // frame as an RNode station ID, so a readable probe turns up in every
  // neighbouring node's table as a station called "RM?". These bytes cannot be
  // mistaken for one, and the split flag is clear so no reassembly starts.
  const uint8_t probe[] = { 0x00, 0x01, 0x02, 0x03 };
  const uint32_t started = millis();
  ulTaskNotifyTake(pdTRUE, 0);                   // clear anything stale
  LoRaFem::tx();                                 // the probe proves the front end too
  if (_radio->startTransmit((uint8_t*)probe, sizeof(probe)) != RADIOLIB_ERR_NONE) {
    log_w("radio self-test: could not start a transmission — skipping the IRQ check");
    s_taskHandle = previous;
    LoRaFem::rx();
    _radio->startReceive();
    return;
  }
  const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
  const uint32_t took = millis() - started;
  s_taskHandle = previous;
  _radio->finishTransmit();
  LoRaFem::rx();
  _radio->startReceive();

  // Which pin is the interrupt depends on the family: DIO1 on an SX126x or
  // SX128x, DIO0 on an SX127x. Naming the wrong one turns a useful diagnostic
  // into a misleading one.
  const int irqPin = _sx1276 ? PIN_LORA_DIO0 : PIN_LORA_DIO1;
  if (got) {
    log_i("radio self-test: TxDone interrupt arrived in %lu ms — the IRQ line on GPIO %d is live",
          (unsigned long)took, irqPin);
  } else {
    // Worth being blunt. Everything else about this node will look healthy.
    log_e("radio self-test: NO TxDone interrupt after %lu ms. The chip transmits but nothing "
          "is watching its IRQ, so this node will never receive a packet. GPIO %d is not the "
          "interrupt pin on this board — check the board header against the schematic.",
          (unsigned long)took, irqPin);
  }
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

  // What the channel is governed by depends on the band it sits in, and the
  // three regimes constrain different things — see Airtime::Regime.
  // The stored region decides which rulebook applies. Frequency is only used to
  // infer one for a node configured before the setting existed — otherwise
  // choosing "custom" at 868 MHz would tell the operator no plan applied while
  // the European duty cycle went on being enforced underneath.
  const Airtime::RegionInfo* region = Airtime::regionFor(s.region, s.freqMhz);
  const Airtime::Regime regime = region->regime;
  const uint16_t limit = Airtime::effectiveBasisPoints(regime, s.freqMhz, s.bwKhz, s.dutyCyclePct);
  g_stats.dutyLimitBp = limit;

  switch (regime) {

  case Airtime::Regime::EuSrd868: {
    // The transmit budget belongs to the sub-band, so it is re-derived
    // whenever the channel moves.
    const Airtime::Band* band = Airtime::bandFor(s.freqMhz, s.bwKhz);
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
    } else if (band) {
      log_i("channel is in EU SRD %s — holding to %.2f %% of the hour", band->name, limit / 100.0f);
    }
    break;
  }

  case Airtime::Regime::UsIsm915: {
    // No hourly budget here. What FCC 15.247 caps is how long one transmission
    // may hold the channel, so the check that matters is per packet: the
    // longest frame this firmware will send is a full fragment.
    const uint32_t dwellMs = Airtime::maxDwellMs(regime);
    const float    dtsMin  = Airtime::dtsMinBandwidthKhz(regime);
    const float    worstMs = _airtime.timeOnAirMs(LORA_FRAG_PAYLOAD);

    if (s.bwKhz >= dtsMin) {
      log_i("channel is in the US 902-928 ISM band at %.0f kHz — wide enough to be a digital "
            "transmission system, so the %lu ms hopping dwell limit does not apply; longest "
            "frame is %.0f ms", (double)s.bwKhz, (unsigned long)dwellMs, (double)worstMs);
    } else if (worstMs > (float)dwellMs) {
      // Worth being blunt: this is the configuration most people arrive at by
      // copying an EU channel plan across, and it is the one that cannot comply.
      log_w("US 902-928 ISM: a full %u-byte frame takes %.0f ms at SF%u/%.0f kHz, over the "
            "%lu ms per-channel dwell limit for a non-hopping system. Either widen the channel "
            "to %.0f kHz or more, or drop to a spreading factor that fits.",
            (unsigned)LORA_FRAG_PAYLOAD, (double)worstMs, (unsigned)s.sf, (double)s.bwKhz,
            (unsigned long)dwellMs, (double)dtsMin);
    } else {
      log_i("channel is in the US 902-928 ISM band — longest frame is %.0f ms, inside the "
            "%lu ms dwell limit", (double)worstMs, (unsigned long)dwellMs);
    }
    if (limit) log_i("  a manual %u %% cap is set and will be enforced as well", (unsigned)s.dutyCyclePct);
    break;
  }

  case Airtime::Regime::Ism2400:
    // Neither a duty cycle nor a dwell ceiling: this band is bounded by
    // radiated power and by listen-before-talk, which CSMA already does.
    log_i("channel is in the 2.4 GHz ISM band — no duty cycle applies; CSMA and the power "
          "ceiling are what govern here%s",
          limit ? ", plus the manual cap you have set" : "");
    break;

  default:
    if (limit) log_w("%.3f MHz is outside every band plan this firmware knows: applying the "
                     "configured %u %% limit, check your local rules",
                     (double)s.freqMhz, (unsigned)s.dutyCyclePct);
    else log_w("%.3f MHz is outside every band plan this firmware knows and no duty cycle is "
               "set — transmitting unlimited, which is unlikely to be legal anywhere",
               (double)s.freqMhz);
    break;
  }
}

bool LoRaRadio::probeSX1262(const RadioSettings& s) {
  // Boards with an amplified front end power and point it before the chip is
  // asked anything: the boot self-test transmits, and a probe through a dead
  // front end proves nothing but the front end (LoRaFem.h). A no-op elsewhere.
  LoRaFem::begin();
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, _spi);
  SX1262* sx  = new SX1262(mod);
  int16_t state = sx->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, RadioCaps::kSX1262.txMinDbm,
                                       RadioCaps::kSX1262.txMaxDbm),
                            s.preamble, RF_TCXO_VOLTAGE, false);
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
  _caps = &RadioCaps::kSX1262;
  return true;
}

bool LoRaRadio::probeSX1280(const RadioSettings& s) {
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, _spi);
  SX1280* sx  = new SX1280(mod);
  #if HAS_RF_SWITCH
    // The PA sits behind a transmit/receive switch. RadioLib drives it once it
    // knows the pins; without this the antenna is connected in neither
    // direction and the radio is deaf and mute while reporting itself up.
    sx->setRfSwitchPins(PIN_LORA_RXEN, PIN_LORA_TXEN);
  #endif
  int16_t state = sx->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, RadioCaps::kSX1280.txMinDbm,
                                       RadioCaps::kSX1280.txMaxDbm),
                            s.preamble);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("SX1280 not found (code %d)", state);
    delete sx; delete mod;
    return false;
  }
  // setCRC takes a *length* here, not a flag as it does on the SX126x: two
  // bytes matches what the sub-GHz parts put on the air by default, so the
  // framing above this layer sees the same guarantees on either radio.
  sx->setCRC(2);
  _radio = _sx1280 = sx;
  _modelName = "SX1280";
  _caps = &RadioCaps::kSX1280;
  return true;
}

bool LoRaRadio::probeSX127x(const RadioSettings& s) {
  // SX1276 and SX1278 share silicon version 0x12 and this driver; the
  // class only differs in the accepted frequency range, and SX1276 spans
  // both sub-GHz bands.
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1, _spi);
  SX1276* sx  = new SX1276(mod);
  int16_t state = sx->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, RadioCaps::kSX1276.txMinDbm,
                                       RadioCaps::kSX1276.txMaxDbm),
                            s.preamble, 0);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("SX127x not found (code %d)", state);
    delete sx; delete mod;
    return false;
  }
  sx->setCurrentLimit(140);
  sx->setCRC(true);
  _radio = _sx1276 = sx;
  _modelName = "SX1276";
  _caps = &RadioCaps::kSX1276;
  return true;
}

void LoRaRadio::logActive() const {
  log_i("%s online: %.3f MHz, BW %.1f kHz, SF%d, CR 4/%d, %d dBm, sync 0x%02X, preamble %u",
        _modelName, _active.freqMhz, _active.bwKhz, _active.sf, _active.cr,
        clampPower(_active.txDbm, _caps->txMinDbm, maxTxDbm()), _active.syncWord, _active.preamble);
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
  // through whichever concrete driver was detected. Exactly one pointer is
  // ever set, so the chain resolves to one call.
  #define CHIP(call) (_sx1262 ? _sx1262->call : _sx1280 ? _sx1280->call : _sx1276->call)
  struct Step { const char* what; int16_t code; };
  Step steps[] = {
    { "standby",          _radio->standby() },
    { "frequency",        CHIP(setFrequency(s.freqMhz)) },
    { "bandwidth",        CHIP(setBandwidth(s.bwKhz)) },
    { "spreading factor", CHIP(setSpreadingFactor(s.sf)) },
    { "coding rate",      CHIP(setCodingRate(s.cr)) },
    { "tx power",         CHIP(setOutputPower(clampPower(s.txDbm, _caps->txMinDbm, maxTxDbm()))) },
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
  Watchdog::watch();
  // The radio is the one thing a relay exists to do, so it is the last thing
  // that should be allowed to end the node (Diag.h) — and a guard that let
  // this function *return* would end it just as surely: ESP-IDF's
  // vPortTaskWrapper aborts on a task function that returns, so containing
  // the throw and falling out of the bottom is the same reboot by a longer
  // road. taskLoop() does not return of its own accord; when it throws, this
  // goes back into it.
  for (;;) {
    Diag::guard("the radio task", [self] { static_cast<LoRaRadio*>(self)->taskLoop(); });
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void LoRaRadio::taskLoop() {
  // Off the watchdog before the task goes, not after: radioTask() subscribed
  // us and nothing in IDF clears a subscription when its task is deleted, so
  // a board whose transceiver did not come up — a survivable failure, the AP
  // and the portal stay up and say "radio offline" — would otherwise be reset
  // by a watchdog waiting on a task that no longer exists, every thirty
  // seconds, for ever.
  if (!_online) { Watchdog::unwatch(); vTaskDelete(nullptr); return; }

  _radio->startReceive();
  _lastTxMs  = millis();
  _helloAtMs = millis() + BEACON_HELLO_DELAY_MS;

  for (;;) {
    // Reported here rather than in radioTask's wrapper around this call: this
    // loop does not return, so a feed out there ran once and never again, and
    // the node rebooted itself thirty seconds later on a quiet channel. Found
    // on the bench, which is the only place it is cheap to find.
    Watchdog::feed();
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
// Which bit means "a packet arrived", in the chip's own register.
//
// PhysicalLayer::getIrqFlags() looks generic and is not: every driver returns
// its raw hardware register, and the bits do not line up. RxDone is bit 6 on an
// SX127x and bit 1 on an SX126x or SX128x, so a single generic constant tested
// against all three is right for two of them by coincidence and silently wrong
// for the other — which is exactly what happened here. The 2.4 GHz boards kept
// working while both sub-GHz boards stopped receiving altogether, because on
// those bit 1 is FhssChangeChannel and never set.
uint32_t LoRaRadio::rxDoneFlag() const {
  if (_sx1276) return RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE;   // 0b01000000
  if (_sx1262) return RADIOLIB_SX126X_IRQ_RX_DONE;              // 0b10
  if (_sx1280) return RADIOLIB_SX128X_IRQ_RX_DONE;              // 0x0002
  return 0;
}

void LoRaRadio::handleRadioIrq() {
  // Ask the radio what it actually wants, rather than assuming a notification
  // means a packet arrived.
  //
  // It very often does not. The interrupt line carries TxDone and
  // channel-activity results as well as RxDone, and — the expensive one — a
  // reception whose flags have not been cleared re-raises the line the moment
  // receive mode is re-entered. That turned a single frame into 391 deliveries
  // at a steady 13 ms apart, each one a fresh copy of the same bytes read back
  // out of the chip, until the RX ring overflowed. On a channel measured at
  // 0.67 % occupancy the node was reporting several packets a second and
  // discarding 93 % of them; none of that traffic existed.
  const uint32_t rxDone = rxDoneFlag();
  const uint32_t irq = _radio->getIrqFlags();
  if (rxDone && (irq & rxDone) == 0) {
    g_stats.loraRxSpuriousIrq++;
    _radio->clearIrqFlags(0xFFFFFFFF);           // everything; nothing here is ours
    _radio->startReceive();
    return;
  }

  size_t len = _radio->getPacketLength();
  // At least one payload byte behind the header. A header-only frame used to
  // pass this guard, yield a zero-length payload, and then fall out of the
  // reassembly below without touching any counter — a frame that simply
  // vanished. Expressed against LORA_HEADER_LEN so it stays right if the
  // framing ever grows.
  if (len <= LORA_HEADER_LEN || len > LORA_FRAME_MAX) {
    g_stats.loraRxBadLength++;
    _radio->clearIrqFlags(0xFFFFFFFF);
    _radio->startReceive();
    return;
  }

  int16_t state = _radio->readData(_frame, len);
  if (state != RADIOLIB_ERR_NONE) {      // CRC error or spurious IRQ
    // Counted, because a node hearing hundreds of these an hour is sitting in
    // interference — which looks nothing like a node whose consumer is slow,
    // and used to be indistinguishable from it.
    g_stats.loraRxCrcErrors++;
    _radio->clearIrqFlags(0xFFFFFFFF);
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

  // The reception is collected; drop its flags so re-entering receive mode
  // cannot present the same packet again.
  _radio->clearIrqFlags(0xFFFFFFFF);
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
  // The reading goes with the frame rather than being read from g_stats when
  // the transport drains the ring: a backlog gave every frame in the batch the
  // newest one's RSSI, and that number is the whole of a signal report.
  const LoRaRxFrame hdr{ g_stats.lastRssi, g_stats.lastSnr };
  void* slot = nullptr;
  if (xRingbufferSendAcquire(_rxRing, &slot, sizeof(hdr) + len, 0) == pdTRUE) {
    memcpy(slot, &hdr, sizeof(hdr));
    memcpy((uint8_t*)slot + sizeof(hdr), _rxBuf, len);
    xRingbufferSendComplete(_rxRing, slot);
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

  LoRaFem::tx();                         // amplifier into the path first
  int16_t state = _radio->startTransmit((uint8_t*)frame, len);
  if (state != RADIOLIB_ERR_NONE) {
    LoRaFem::rx();                       // back to listening before anything else
    log_e("startTransmit failed, code %d", state);
    return false;
  }

  // Wait for TxDone. SF12/125k worst case is ~5 s per frame; 8 s means the
  // radio wedged, in which case finishTransmit() cleans up.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(8000));
  // Airtime is progress, and there can be more of it in one pass of taskLoop()
  // than the watchdog allows between feeds: a beacon and a queued packet in
  // the same pass is one deferral plus one frame, then another deferral plus
  // two fragments — 5 + 8 + 5 + 16 s at the bounds above, comfortably past
  // WATCHDOG_TIMEOUT_S. So each frame reports for itself.
  Watchdog::feed();
  _radio->finishTransmit();
  LoRaFem::rx();                         // back to the LNA before listening resumes
  _airtime.addTx(millis(), _airtime.timeOnAirMs(len));
  return true;
}

// One channel-activity-detection probe. scanChannel() blocks for roughly a
// symbol and drives the IRQ line itself; sendFrame() flushes any stray
// notification it leaves behind.
bool LoRaRadio::mediumFree() {
  const int16_t cad = _radio->scanChannel();

  // scanChannel() drives the IRQ line itself, and the notification it leaves
  // behind is indistinguishable from an incoming frame to the waits in
  // csmaWait(). So every probe looked like traffic: the contention countdown
  // saw its own CAD, declared the channel disturbed, reset to zero and probed
  // again — for as long as CSMA_MAX_WAIT_MS allowed. The node was deferring to
  // itself, backing off maximally before every transmission and burning about
  // 1200 pointless task wake-ups doing it.
  //
  // Consuming the notification here is what breaks that loop. A frame that
  // genuinely arrived during the probe still has its RxDone flag set in the
  // chip, so it is collected rather than hidden by the flush.
  ulTaskNotifyTake(pdTRUE, 0);
  const uint32_t rxDone = rxDoneFlag();
  if (rxDone && (_radio->getIrqFlags() & rxDone)) handleRadioIrq();

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
    // Deferring to a busy channel is progress too, and this loop can hold the
    // task for CSMA_MAX_WAIT_MS on its own before a byte is sent.
    Watchdog::feed();
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
  // Same rulebook as configureAirtime() chose, or this would quietly re-apply
  // the European budget to a node the operator had put in another region.
  const Airtime::RegionInfo* rg = Airtime::regionFor(_active.region, _active.freqMhz);
  const uint16_t limit = Airtime::effectiveBasisPoints(rg->regime, _active.freqMhz,
                                                       _active.bwKhz, _active.dutyCyclePct);
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
