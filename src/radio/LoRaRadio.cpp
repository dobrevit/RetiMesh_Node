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
#include "RadioSelfTestPolicy.h"
#include <esp_app_desc.h>
#include <esp_random.h>
#include <Preferences.h>
#include "Neighbors.h"
#include "WifiManager.h"
#include "Watchdog.h"
#include "SpiBus.h"

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
// Why the radio task woke
// ---------------------------------------------------------------------------
// Two senders share the task's notification: the ISR below, on the chip's IRQ
// line, and any task that has queued work for the radio (wake()). The value
// carries a bit per sender rather than a count, because every wait in this file
// has to know which of the two it heard, and a count cannot say.
//
// It is not a cosmetic distinction. A producer's wake read as an interrupt
// would be charged to rx_spurious_irq by handleRadioIrq() — a counter that
// reads zero on a healthy node and is watched by the soak rig — and, worse,
// csmaWait() would read it as traffic on the channel and restart a contention
// countdown that nothing had disturbed. That second failure has already been
// paid for once here, when the old blocking channel scan left a notification
// behind that the contention countdown could not tell from an incoming frame
// and every node deferred to itself for the whole of CSMA_MAX_WAIT_MS.
//
// A bit is also the only carrier that cannot race. The notification value and
// the "a notification is pending" state are updated together under the
// kernel's own lock, so a wake and its reason arrive as one; a flag beside the
// notification, however it is ordered, has a window in which the two disagree.
static const uint32_t kWakeIrq  = 1u << 0;   // the transceiver raised its line
static const uint32_t kWakeWork = 1u << 1;   // a producer queued something

// How long the task parks when there is nothing to do. It is a fallback, not a
// poll: an arriving frame wakes it through the ISR and queued work wakes it
// through wake(), so this bound is only what covers the things nobody signals
// — the beacon clock, the airtime figures, and the watchdog feed at the top of
// the pass. Widened from 10 ms in round 3: at 10 ms the task woke a hundred
// times a second on an idle channel to find nothing, which is the cost this
// removes. Do not shorten it back "to reduce latency" — the two things that
// have latency are woken, not polled.
static const uint32_t kIdleWaitMs = 100;

// The pass has to feed the watchdog more often than it fires, and the park is
// the only unbounded-looking wait on the path between two feeds.
static_assert(kIdleWaitMs < (uint32_t)WATCHDOG_TIMEOUT_S * 1000UL,
              "the idle park must fit between two watchdog feeds");

// ---------------------------------------------------------------------------
// ISR: the radio's IRQ line (DIO1 on SX126x, DIO0 on SX127x) rises on
// RxDone / TxDone. Nothing is decided here — the task owns the radio and
// knows (via its own state) which operation finished.
// ---------------------------------------------------------------------------
void IRAM_ATTR LoRaRadio::onRadioIrq() {
  BaseType_t higherPrioWoken = pdFALSE;
  if (s_taskHandle) xTaskNotifyFromISR(s_taskHandle, kWakeIrq, eSetBits, &higherPrioWoken);
  portYIELD_FROM_ISR(higherPrioWoken);
}

// Wait for the chip's IRQ line, and say whether that is what ended the wait.
//
// A wake for queued work is not what any caller of this is waiting for, so it
// is left in the notification value for the park in taskLoop() to collect — a
// packet queued while this task was busy is still serviced the moment it comes
// back round. Only the interrupt bit is consumed here.
bool LoRaRadio::waitIrq(uint32_t ms) {
  uint32_t bits = 0;
  if (xTaskNotifyWait(0, kWakeIrq, &bits, pdMS_TO_TICKS(ms)) != pdPASS) return false;
  return (bits & kWakeIrq) != 0;
}

// Drop an interrupt notification left over from an operation that has finished,
// so the next wait cannot read it as its own. A zero-length wait is the whole
// implementation: it clears the bit whether or not one was pending, and clears
// the pending state with it. A queued-work wake is deliberately not touched.
void LoRaRadio::flushIrq() {
  xTaskNotifyWait(kWakeIrq, kWakeIrq, nullptr, 0);
}

// ---------------------------------------------------------------------------
// "There is something for you" — the producer side of the park
// ---------------------------------------------------------------------------
// Called after work has been made visible to the task: a packet put in the TX
// ring, a settings change handed over, a sleep requested. Order matters and is
// the caller's half of the contract — publish the work first, then call this —
// because the two halves interlock:
//
//   * the task sets _parked before it looks at the TX ring, and only parks if
//     the ring is empty. So a producer that finds _parked set is talking to a
//     task that is either in the wait or about to enter it, and the wake is
//     seen either way: a notification that arrives first is already pending
//     when the wait starts and returns from it immediately.
//   * a producer that finds it clear is talking to a task that has not yet
//     reached that point in its pass, and every path to it passes the sleep
//     and reconfigure checks and the ring. It will find the work on this pass
//     without being told.
//
// Not sent while the task is busy, and that is the point rather than an
// economy: the waits inside csmaWait() count out a contention window in slots,
// and a wake delivered into one would cost a slot that was never waited.
//
// Safe before the task exists and after it has gone: a radio that never came
// up deletes its task at the top of taskLoop() and clears the handle first,
// and _parked is false in both cases.
void LoRaRadio::wake() {
  if (!_parked) return;
  TaskHandle_t h = s_taskHandle;
  if (h) xTaskNotify(h, kWakeWork, eSetBits);
}

// ---------------------------------------------------------------------------
bool LoRaRadio::begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, const RadioSettings& s) {
  _txRing = txRing;
  _rxRing = rxRing;
  _active = s;

  _spi = &SpiBus::get(LORA_SPI_BUS, PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);

#if RF_MODEM_LR1110
  // The same correction the SX1280 needs, for a narrower reason: this part is
  // sub-GHz like the SX126x, so a stored frequency is usually fine — but its
  // bandwidth list has four entries where the SX126x has ten, and a node moved
  // across from one keeps the old figure in NVS. Left alone, begin() fails on
  // the bandwidth and the log blames the wiring.
  // Only the field that is actually unusable, unlike the SX1280 below. There
  // the whole channel is wrong by construction — a sub-GHz plan names nothing
  // a 2.4 GHz part can tune — so replacing it wholesale says what happened.
  // Here the frequency and the spreading factor usually survive the move and
  // the bandwidth usually does not, and rewriting all three would carry an
  // operator off the frequency they chose to fix a bandwidth they did not.
  {
    const RadioCaps::Caps& c = RadioCaps::kLR1110;
    const bool badFreq = _active.freqMhz < c.freqMinMhz || _active.freqMhz > c.freqMaxMhz;
    const bool badBw   = !RadioCaps::bandwidthSupported(c, _active.bwKhz);
    const bool badSf   = _active.sf < c.sfMin || _active.sf > c.sfMax;
    if (badFreq || badBw || badSf) {
      log_w("stored channel %.3f MHz / %.1f kHz / SF%u is not one an LR1110 can run "
            "(%s%s%s) — replacing only that, the rest stands",
            (double)_active.freqMhz, (double)_active.bwKhz, (unsigned)_active.sf,
            badFreq ? "frequency " : "", badBw ? "bandwidth " : "", badSf ? "spreading factor" : "");
      if (badFreq) _active.freqMhz = RF_FREQ_MHZ;
      if (badBw)   _active.bwKhz   = RF_BW_KHZ;
      if (badSf)   _active.sf      = RF_SF;
    }
  }
  if (!probeLR1110(_active)) {
    log_e("No LR1110 found on IRQ(DIO9)=%d/BUSY=%d — check wiring", PIN_LORA_DIO1, PIN_LORA_BUSY);
    LoRaFem::off();                              // begin() never fails with the front end up
    return false;
  }
#elif RF_MODEM_SX1280
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
    LoRaFem::off();                              // begin() never fails with the front end up
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
    // begin() never returns false with the front end up. probeSX1262() powers
    // it before asking the chip anything — the self-test transmits, and a probe
    // through a dead front end proves nothing — and a probe that then fails
    // leaves _online false, so the radio task deletes itself at the top of
    // taskLoop() and nothing ever reaches enterSleep()'s LoRaFem::off(). The
    // rail would stay up with the LNA in the path for as long as the node runs,
    // on the one board where that amplifier is the larger idle draw. A no-op
    // where there is no front end, and before begin() has run (LoRaFem.h).
    LoRaFem::off();
    return false;
  }
#endif

  _radio->setPacketReceivedAction(onRadioIrq);   // DIO1 / DIO0 as appropriate
  _online = true;
  g_stats.radioModel = _modelName;
  configureAirtime(_active);                     // the probes bypass applySettings()
  logActive();
  #if RADIO_SELFTEST_ON_BOOT
    bootSelfTest();
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
//
// Returns whether the line answered. Only a true is worth remembering, which
// is bootSelfTest()'s business; this function's is the chip and the pin.
bool LoRaRadio::irqSelfTest() {
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
  flushIrq();                                    // clear anything stale
  LoRaFem::tx();                                 // the probe proves the front end too
  if (_radio->startTransmit((uint8_t*)probe, sizeof(probe)) != RADIOLIB_ERR_NONE) {
    log_w("radio self-test: could not start a transmission — skipping the IRQ check");
    s_taskHandle = previous;
    LoRaFem::rx();
    _radio->startReceive();
    return false;                                // nothing was proved
  }
  const bool got = waitIrq(3000);
  const uint32_t took = millis() - started;
  s_taskHandle = previous;
  _radio->finishTransmit();
  LoRaFem::rx();
  _radio->startReceive();

  if (got) {
    log_i("radio self-test: TxDone interrupt arrived in %lu ms — the IRQ line on GPIO %d is live",
          (unsigned long)took, irqPin());
  } else {
    // Worth being blunt. Everything else about this node will look healthy.
    log_e("radio self-test: NO TxDone interrupt after %lu ms. The chip transmits but nothing "
          "is watching its IRQ, so this node will never receive a packet. GPIO %d is not the "
          "interrupt pin on this board — check the board header against the schematic.",
          (unsigned long)took, irqPin());
  }
  return got;
}

#if RADIO_SELFTEST_ON_BOOT
// ...and once per firmware image rather than once per boot.
//
// The airtime is worth paying to answer a question, and worthless to answer it
// again: the pin map the test proves belongs to the image, so an image that has
// already answered on this node has nothing left to find out. The case this
// exists for is a solar node brown-out looping at dawn, spending a transmission
// on every cycle out of the supply that could not hold the last boot up — and
// no boot-reason check can help there, because a brown-out is not a clean boot
// and the RTC domain that would carry the state does not survive the rail
// dropping. So the verdict goes to NVS, keyed to the image (RadioSelfTestPolicy.h).
//
// Its own namespace: this is not a setting, so a settings reset must not clear
// it, and it is not restart history either. Flashing any different image is
// what asks the question again — which is also exactly when the answer can
// have changed.
void LoRaRadio::bootSelfTest() {
  // What identifies the image is the image, not what it calls itself: the
  // application descriptor carries the SHA-256 of the ELF it was built from,
  // and esptool patches it into the binary at elf2image time, so it differs
  // between two builds that share a version string. FW_VERSION does not —
  // see RadioSelfTestPolicy.h for why that difference is the whole point.
  const esp_app_desc_t* desc = esp_app_get_description();
  const uint32_t image = RadioSelfTest::buildMark(desc->app_elf_sha256,
                                                  sizeof(desc->app_elf_sha256));
  // The field is filled in after the link, by esptool, and this build path
  // leaves it to find the descriptor on its own rather than being told where it
  // is. A toolchain that stopped patching leaves 32 zero bytes there — on every
  // image alike — so the policy answers NO_MARK and the two calls below fall
  // back to running the test every boot (RadioSelfTestPolicy.h). That is the
  // safe direction and it is also invisible, so it is said out loud: a node
  // paying a transmission per boot for ever deserves a reason in its log.
  if (image == RadioSelfTest::NO_MARK)
    log_w("radio self-test: this image carries no ELF hash in its application descriptor, so it "
          "cannot be told apart from any other — running the test every boot. The build's "
          "elf2image step is not stamping the descriptor.");

  Preferences p;
  const bool store = p.begin(RADIO_NVS_NAMESPACE, false);
  // Guarded like every other read in this firmware: Preferences logs an error
  // for a key that is not there, and on a fresh node it never is.
  const uint32_t stored = (store && p.isKey(RADIO_SELFTEST_NVS_KEY))
                            ? p.getUInt(RADIO_SELFTEST_NVS_KEY, RadioSelfTest::NO_MARK)
                            : RadioSelfTest::NO_MARK;

  if (RadioSelfTest::proven(stored, image)) {
    // The one line that says which path was taken. The run path says so
    // itself, in more detail, from irqSelfTest().
    log_i("radio self-test: skipped — this exact image already proved "
          "the IRQ line on GPIO %d", irqPin());
    if (store) p.end();
    return;
  }

  const uint32_t mark = RadioSelfTest::markAfter(irqSelfTest(), image);
  if (store) {
    if (mark != RadioSelfTest::NO_MARK)        p.putUInt(RADIO_SELFTEST_NVS_KEY, mark);
    else if (stored != RadioSelfTest::NO_MARK) p.remove(RADIO_SELFTEST_NVS_KEY);
    p.end();
  }
  // A namespace that would not open costs one transmission per boot and
  // nothing else: the test runs, as it did before any of this existed.
}
#endif

// The TCXO ramp RadioLib programs into an SX126x, which decides how long a
// receive sleep has to be before the driver will take it. Nothing here passes a
// delay to setTCXO(), so a board that names a voltage gets the driver's own
// default; a board that names none never has setTCXO() called at all and the
// delay stays at zero. Compile-time because it is a fact about the board.
static constexpr uint32_t kTcxoDelayUs =
    (RF_TCXO_VOLTAGE > 0.0f) ? Airtime::RX_DC_TCXO_DELAY_US : 0;

// Airtime maths follows the channel: symbol time drives both the duty-cycle
// accounting and the CSMA slot length. RadioLib leaves CRC and the explicit
// header on, which is what RNode-compatible framing expects.
void LoRaRadio::configureAirtime(const RadioSettings& s) {
  Airtime::Params ap;
  ap.sf = s.sf; ap.bwKhz = s.bwKhz; ap.cr = s.cr;
  ap.preambleSyms = s.preamble; ap.crcOn = true; ap.implicitHeader = false;
  _airtime.configure(ap);
  g_stats.csmaSlotMs = (uint16_t)_airtime.slotMs();

  // What duty-cycled receive would do on this channel. Three things have to
  // agree — the operator asked for it, the fitted chip has the mode, and the
  // sleep the channel yields is long enough for the driver to bother — and at
  // the shipped SF8/125 kHz default the third does not hold. Reported rather
  // than the setting, so the node never claims a saving it is not making.
  // Recomputed here because this runs on every apply, which is the only time
  // any of the three can change. The arithmetic is Airtime's; this is a caller.
  //
  // Two answers rather than one, derived from the same expression so they can
  // never disagree: what the chip and channel could do, and that narrowed by
  // what the operator asked for. A surface with only the second cannot say why
  // it is false, and with the setting shipping off that is every node.
  const uint32_t rxDcSleepUs = Airtime::rxDutyCycleSleepUs(s.sf, s.bwKhz, s.preamble);
  const bool rxDcWouldEngage = _caps->rxDutyCycle &&
                               Airtime::rxDutyCycleEngages(rxDcSleepUs, kTcxoDelayUs);
  g_stats.rxDutyCycleSleepUs     = rxDcSleepUs;
  g_stats.rxDutyCycleWouldEngage = rxDcWouldEngage;
  g_stats.rxDutyCycleEngages     = s.rxDutyCycle && rxDcWouldEngage;
  // Armed-ness is a separate fact from all of the above, and here it is simply
  // false: nothing in this file calls startReceiveDutyCycleAuto(), every path
  // arms a plain startReceive(). The release that arms the mode sets this where
  // it arms it, and flips a published value rather than editing a caveat out of
  // the documentation.
  g_stats.rxDutyCycleArmed = false;

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
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, *_spi);
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

#if RF_MODEM_LR1110
// The LR1110's antenna switch is not on the MCU's GPIOs: the chip drives it
// itself from its own DIO lines, and it has to be told which line does what
// before it will connect the antenna in either direction. Until then the part
// answers over SPI, reports a version, accepts a channel and transmits into a
// disconnected pin — online by every measure this firmware has, and silent.
// That is the same failure the amplified Heltec V4 has, arriving by a
// different route, and it is why this radio is declared by the board rather
// than probed for: the table is board wiring, not chip behaviour.
//
// The board supplies it as LR11X0_RF_SWITCH_DIOS / LR11X0_RF_SWITCH_TABLE.
static const uint32_t kRfSwitchDios[Module::RFSWITCH_MAX_PINS] = LR11X0_RF_SWITCH_DIOS;
static const Module::RfSwitchMode_t kRfSwitchTable[] = LR11X0_RF_SWITCH_TABLE;

bool LoRaRadio::probeLR1110(const RadioSettings& s) {
  // The fourth argument is BUSY, as on the SX126x; the second is the interrupt,
  // which on this part is DIO9 rather than DIO1.
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, *_spi);
  LR1110* lr  = new LR1110(mod);
  int16_t state = lr->begin(s.freqMhz, s.bwKhz, s.sf, s.cr, s.syncWord,
                            clampPower(s.txDbm, RadioCaps::kLR1110.txMinDbm,
                                       RadioCaps::kLR1110.txMaxDbm),
                            s.preamble, RF_TCXO_VOLTAGE);
  if (state != RADIOLIB_ERR_NONE) {
    log_w("LR1110 not found (code %d)", state);
    delete lr; delete mod;
    return false;
  }
  // After begin(), not before: the table is written to the chip, and before
  // begin() there is nothing there to write it to.
  lr->setRfSwitchTable(kRfSwitchDios, kRfSwitchTable);
  // A length here, as on the SX1280 — the sub-GHz SX126x takes a flag. For
  // LoRa the part only cares that it is non-zero, but two bytes is what the
  // other radios put on the air, and the framing above this layer should not
  // be able to tell which chip it is running on.
  lr->setCRC(2);
  lr->explicitHeader();

  // What is actually in the package, since this part carries a firmware of its
  // own and the version is the one number that explains a chip which answers
  // but misbehaves.
  LR11x0VersionInfo_t v;
  if (lr->getVersionInfo(&v) == RADIOLIB_ERR_NONE)
    log_i("LR1110 firmware %u.%u (hardware %u, device %u)",
          (unsigned)v.fwMajor, (unsigned)v.fwMinor,
          (unsigned)v.hardware, (unsigned)v.device);

  _radio = _lr1110 = lr;
  _modelName = "LR1110";
  _caps = &RadioCaps::kLR1110;
  return true;
}
#endif // RF_MODEM_LR1110

bool LoRaRadio::probeSX1280(const RadioSettings& s) {
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, *_spi);
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
  Module* mod = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1, *_spi);
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
  // Handed over first, then asked for: a settings page that waited out the
  // park would take a tenth of a second to answer, and the flag has to be
  // readable by the time the wake arrives.
  wake();
}

// ---------------------------------------------------------------------------
// Shutdown: the last thing the radio task does before the node restarts
// ---------------------------------------------------------------------------
void LoRaRadio::requestSleep() {
  portENTER_CRITICAL(&_mux);
  _sleepRequest = true;
  portEXIT_CRITICAL(&_mux);
  // The restart gives the radio a fixed 250 ms (Bootloader.cpp) and every
  // millisecond of it is the transceiver still drawing. A parked task answers
  // at once rather than when its wait runs out; a busy one is already reading
  // this flag between CSMA slots, which is the granularity that bound was
  // sized against and is unchanged by the longer park.
  wake();
}

bool LoRaRadio::asleep() const {
  // A radio that never came up has nothing to put to sleep — and its task
  // deleted itself at the top of taskLoop(), so nobody is left to answer the
  // request. Saying "asleep" here is what stops the caller spending its whole
  // bound waiting for a chip that is not there.
  return _asleep || !_online;
}

// Radio task context only. Nothing wakes the chip again: the caller is the
// restart's quiesce step and what follows it is esp_restart().
void LoRaRadio::enterSleep() {
  portENTER_CRITICAL(&_mux);
  _sleepRequest = false;
  portEXIT_CRITICAL(&_mux);

  const int16_t state = _radio->sleep();
  // The front end goes after the chip, not before: an amplifier whose rail is
  // pulled while the transceiver is still driving its antenna pin is the one
  // ordering that could stress the part.
  LoRaFem::off();
  // Under the lock like every other write to the pair: quiesce() polls asleep()
  // from another task and this flag is what ends its wait.
  portENTER_CRITICAL(&_mux);
  _asleep = true;
  portEXIT_CRITICAL(&_mux);

  if (state == RADIOLIB_ERR_NONE) log_i("radio asleep for the restart");
  else log_w("radio would not sleep for the restart (code %d); restarting anyway", state);
}

// Called from the radio task only. Leaves the chip in standby; the caller
// re-arms receive. On failure the previous settings are restored.
bool LoRaRadio::applySettings(const RadioSettings& s) {
  // The LoRa modulation setters are not part of PhysicalLayer, so they go
  // through whichever concrete driver was detected. Exactly one pointer is
  // ever set, so the chain resolves to one call.
  #if RF_MODEM_LR1110
    // This build has exactly one radio in it and the board named it, so the
    // chain is one call either way — but the LR1110 is not an SX126x and its
    // pointer is only declared on the builds that carry one.
    #define CHIP(call) (_lr1110->call)
  #else
    #define CHIP(call) (_sx1262 ? _sx1262->call : _sx1280 ? _sx1280->call : _sx1276->call)
  #endif
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
  // The handle goes before the task does, so wake() cannot notify something
  // that is no longer there. Nothing has been able to reach it yet — no
  // interrupt is attached on a board whose transceiver did not answer, and
  // _parked is false — but a handle to a deleted task outliving the task is
  // the kind of thing a later producer finds by crashing on it.
  if (!_online) {
    s_taskHandle = nullptr;
    Watchdog::unwatch();
    vTaskDelete(nullptr);
    return;
  }

  // Not before the sleep check below, and not unconditionally: radioTask() puts
  // this function back on its feet after a Diag::guard() catch, so a throw on a
  // node that had already gone to sleep for a restart would re-enter here and
  // re-arm continuous receive — on the V4 with the LNA rail up behind it — for
  // however long the ROM downloader is left sitting there.
  if (!_asleep) {
    _radio->startReceive();
    _lastTxMs  = millis();
    _helloAtMs = millis() + BEACON_HELLO_DELAY_MS;
  }

  for (;;) {
    // Reported here rather than in radioTask's wrapper around this call: this
    // loop does not return, so a feed out there ran once and never again, and
    // the node rebooted itself thirty seconds later on a quiet channel. Found
    // on the bench, which is the only place it is cheap to find.
    Watchdog::feed();

    // (0) A restart is being prepared? Sleep the chip, stand the front end
    //     down, and then stay out of the way: keep feeding the watchdog, but
    //     touch nothing. There is no path back — quiesce() asks for this and
    //     esp_restart() follows it.
    if (_sleepRequest) enterSleep();
    if (_asleep) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    // (1) Settings changed from the web UI? Apply between packets.
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

    // (2) Park until there is something to do. Three things end this wait, and
    //     naming them is the point: the chip's IRQ line, through the ISR, which
    //     is how a received frame gets serviced; a producer calling wake()
    //     after putting a packet in the TX ring or asking for a reconfigure or
    //     a sleep; and, failing both, kIdleWaitMs. The timeout is not how work
    //     is found — it is the clock for the things nobody signals, the beacon
    //     schedule and the airtime figures below and the watchdog feed above.
    //
    //     _parked is set before the ring is looked at and cleared after the
    //     wait, which is the task's half of wake()'s interlock: a producer that
    //     sees it set gets a wake through, and one that sees it clear is
    //     talking to a task that has not reached the ring yet. So an item that
    //     arrives in the gap is either found by the ring check here or woken
    //     for, never left to wait out the timeout.
    //
    //     The ring is only asked about when this pass could act on the answer.
    //     While the transmit budget is spent the packets stay in it by design,
    //     and a park that skipped itself for a ring nothing is going to drain
    //     would spin this task at priority 5 for the rest of the hour.
    _parked = true;
    UBaseType_t queued = 0;
    if (!g_stats.dutyLocked)
      vRingbufferGetInfo(_txRing, nullptr, nullptr, nullptr, nullptr, &queued);
    uint32_t bits = 0;
    const bool notified = xTaskNotifyWait(0, kWakeIrq | kWakeWork, &bits,
                                          queued ? (TickType_t)0
                                                 : pdMS_TO_TICKS(kIdleWaitMs)) == pdPASS;
    _parked = false;
    if (notified && (bits & kWakeIrq)) handleRadioIrq();

    // (2a) Channel-use figures, and the duty-cycle verdict they feed.
    refreshAirtimeStats();

    // (2b) Beacons: boot hello, pending reply, periodic id when idle.
    //      At most one beacon per loop pass, and the idle check reads the
    //      clock fresh — a transmission above would otherwise make the
    //      stale `now` minus _lastTxMs wrap and fire immediately.
    if (_active.beaconInterval > 0 && !g_stats.dutyLocked) {
      uint32_t now = millis();
      if (_helloAtMs && (int32_t)(now - _helloAtMs) >= 0)      { _helloAtMs = 0; sendBeacon('H'); }
      else if (_replyAtMs && (int32_t)(now - _replyAtMs) >= 0) { _replyAtMs = 0; sendBeacon('R'); }
      else if ((int32_t)(now - _lastTxMs) >= (int32_t)_active.beaconInterval * 1000) sendBeacon('I');
    }

    // (3) RNS -> LoRa: pull one complete RNS packet from the ring buffer
    //     (put there by LoRaRnsInterface::send_outgoing, which then calls
    //     wake()) and transmit it. One item per pass, non-blocking take: RX
    //     keeps priority, and the next pass skips its park while more remain.
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
#if RF_MODEM_LR1110
  if (_lr1110) return RADIOLIB_LR11X0_IRQ_RX_DONE;              // 1 << 3
#endif
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
  //
  // Every path out of here re-arms receive, and that is the acknowledgement
  // too. Each driver clears the chip's whole interrupt status inside
  // startReceive(), in the RX branch of its stageMode(), before it puts the
  // part back into receive: SX126x and SX128x call clearIrqStatus(), SX127x
  // writes RADIOLIB_SX127X_FLAGS_ALL, LR11x0 clears RADIOLIB_LR11X0_IRQ_ALL.
  // So an explicit clear on each path — which this handler used to make — was
  // a second SPI write of the same register a few microseconds ahead of the
  // driver's own. Named here rather than left to be rediscovered, because a
  // driver that stopped doing it is one lost acknowledgement away from a
  // receiver that never fires again.
  const uint32_t rxDone = rxDoneFlag();
  const uint32_t irq = _radio->getIrqFlags();
  if (rxDone && (irq & rxDone) == 0) {
    g_stats.loraRxSpuriousIrq++;
    _radio->startReceive();                      // ...and the flags with it
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

  // The reception is collected; re-entering receive mode drops its flags, so
  // it cannot be presented again.
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
  // An abandoned transmission counted nothing, so there is nothing to correct:
  // the decrement below is undoing transmitPacket's own increment, and applied
  // to a packet that never went out it would wrap the counter to 4 294 967 295.
  if (!transmitPacket(frame, RNS_BEACON_HDR_LEN + (size_t)n)) return;
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
// Returns false when the packet was abandoned rather than worked through. The
// case that matters is a restart: Bootloader::quiesce() gives the radio 250 ms
// to sleep and then goes regardless, and this call can hold the task far longer
// than that — csmaWait() alone is bounded at CSMA_MAX_WAIT_MS plus the one
// probe that may have started just under it, about 9 s at the worst channel,
// and two fragments are two 8 s waits on top of that. A restart that arrives
// mid-transmit therefore used to end with the node in the ROM downloader, where
// nothing runs and nothing will restart it, with the transceiver still in
// continuous receive and the V4's LNA rail up behind it — indefinitely.
// Dropping the packet is the right trade: the node is going down either way,
// and the sender re-sends.
//
// A true is therefore weaker than "every byte reached the air": a sendFrame()
// that fails mid-packet breaks out of the fragment loop and still returns true,
// because everything after that point — the counter, the idle-beacon clock,
// re-arming receive — is what the caller needs either way and the frame that
// failed has already been logged by sendFrame() itself. Nothing reads this
// return to decide whether to re-send; the one caller reads it to know whether
// a restart cut in.
bool LoRaRadio::transmitPacket(const uint8_t* data, size_t len) {
  if (len == 0 || len > sizeof(_rxBuf)) return false;

  csmaWait();                            // returns early if a sleep is asked for

  // RNode framing: one random sequence nibble for all fragments of this
  // packet, FLAG_SPLIT set when the payload spans more than one frame.
  uint8_t header = (uint8_t)(esp_random() & 0xF0);
  if (len > LORA_FRAG_PAYLOAD) header |= LORA_FLAG_SPLIT;

  size_t offset = 0;
  while (offset < len) {
    // Between fragments, never inside one: a frame that stops mid-air is a
    // frame every listener has to time out on, and the point of stopping here
    // is to be quick. Nothing is left half-written — the ring item is returned
    // by the caller either way, _txFrame is scratch, and the reassembly state
    // this touches is the receiver's, not ours. The chip is left in standby by
    // the last sendFrame(); enterSleep() puts it under on the next pass, which
    // is where it was going.
    if (_sleepRequest) {
      log_w("restarting: abandoning a transmission with %u of %u bytes sent",
            (unsigned)offset, (unsigned)len);
      // _lastTxMs is deliberately left where it was, even though a fragment may
      // have gone out above and been charged to the airtime record. Nothing
      // reads it again on this path: the only reader is the idle-beacon clock
      // in taskLoop(), and what follows this is enterSleep() and esp_restart()
      // — there is no wake. Should a sleep ever gain one, this needs the stamp,
      // or the node comes back believing it has been silent for however long
      // the restart took and beacons immediately.
      return false;
    }
    size_t chunk = min((size_t)LORA_FRAG_PAYLOAD, len - offset);
    _txFrame[0] = header;
    memcpy(_txFrame + 1, data + offset, chunk);
    if (!sendFrame(_txFrame, chunk + LORA_HEADER_LEN)) break;
    offset += chunk;
  }

  g_stats.loraTxPackets++;
  _lastTxMs = millis();
  _radio->startReceive();                // back to listening
  return true;
}

bool LoRaRadio::sendFrame(const uint8_t* frame, size_t len) {
  flushIrq();                            // drop a stale interrupt notification

  LoRaFem::tx();                         // amplifier into the path first
  int16_t state = _radio->startTransmit((uint8_t*)frame, len);
  if (state != RADIOLIB_ERR_NONE) {
    LoRaFem::rx();                       // back to listening before anything else
    log_e("startTransmit failed, code %d", state);
    return false;
  }

  // Wait for TxDone. SF12/125k worst case is ~5 s per frame; 8 s means the
  // radio wedged, in which case finishTransmit() cleans up.
  waitIrq(8000);
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

// Which bits mean "the channel scan finished", in the chip's own register.
//
// The same trap rxDoneFlag() documents, for the other event: getIrqFlags()
// hands back the raw hardware register and the bits do not line up between
// parts — CAD-done is bit 2 on an SX127x, bit 7 on an SX126x, bit 12 on an
// SX128x and bit 8 on an LR11x0. Both bits of each pair are named rather than
// only "done", because the SX127x raises detected alongside done and a mask
// that watched for one bit of a two-bit answer would be a scan this code kept
// waiting for after the chip had already given it.
//
// RadioLib owns this table too, and the alternative is worth naming rather than
// leaving to be rediscovered: PhysicalLayer::getIrqMapped((1UL <<
// RADIOLIB_IRQ_CAD_DONE) | (1UL << RADIOLIB_IRQ_CAD_DETECTED)) returns the
// identical mask on all four parts and cannot drift on a library bump. It is
// not used here for two reasons. It mirrors rxDoneFlag(), which is written
// explicitly because a generic constant tested against three parts is what
// broke reception in the first place; and it is a pure function of the part,
// where the library's map is state — SX127x fills its irqMap inside begin(),
// so the mask reads zero before it and beginFSK() overwrites the CAD entries
// with RADIOLIB_IRQ_NOT_SUPPORTED. The host test pins this table against the
// library's own map for the three parts that populate it in their constructor
// (test/test_radio_irq_flags).
uint32_t LoRaRadio::cadDoneFlag() const {
  if (_sx1276) return RADIOLIB_SX127X_CLEAR_IRQ_FLAG_CAD_DONE |
                      RADIOLIB_SX127X_CLEAR_IRQ_FLAG_CAD_DETECTED;
  if (_sx1262) return RADIOLIB_SX126X_IRQ_CAD_DONE | RADIOLIB_SX126X_IRQ_CAD_DETECTED;
  if (_sx1280) return RADIOLIB_SX128X_IRQ_CAD_DONE | RADIOLIB_SX128X_IRQ_CAD_DETECTED;
#if RF_MODEM_LR1110
  if (_lr1110) return RADIOLIB_LR11X0_IRQ_CAD_DONE | RADIOLIB_LR11X0_IRQ_CAD_DETECTED;
#endif
  return 0;
}

// One relationship the deadline has to hold that Airtime cannot check on its
// own, because it is between two files' constants: a single probe must not be
// able to outlast the deferral it is one step of, nor the watchdog it is being
// fed against. Compile-time, so a channel setting can never reach a state the
// arithmetic forbids.
static_assert(Airtime::CAD_TIMEOUT_MAX_MS < CSMA_MAX_WAIT_MS,
              "one CAD probe must not outlast the whole CSMA deferral");
static_assert(Airtime::CAD_TIMEOUT_MAX_MS < (uint32_t)WATCHDOG_TIMEOUT_S * 1000UL,
              "one CAD probe must fit between two watchdog feeds");

// How often a CAD failure may reach the log. See cadWarnDue().
static const uint32_t kCadWarnIntervalMs = 60000;

// Loud enough to be noticed, bounded enough to be safe to emit from the task
// that is trying to transmit.
//
// The failure this reports is not a one-off: a chip that has stopped answering
// fails every probe, and one deferral is about fifty of them (CSMA_MAX_WAIT_MS
// over a deadline plus CSMA_CAD_RETRY_MS), so a line per probe would be fifty
// blocking console writes for every packet sent. The first failure of each kind
// is reported the moment it happens and after that at most one line a minute
// from either, each carrying its running total so the rate is readable from the
// log alone. The exact figures are cad_timeouts and cad_arm_errors on the
// STATUS line and in /api/status: a fault this quiet has to be visible whether
// or not anyone was watching the log at the time.
bool LoRaRadio::cadWarnDue(uint32_t count) {
  const uint32_t now = millis();
  if (count > 1 && now - _cadWarnAtMs < kCadWarnIntervalMs) return false;
  _cadWarnAtMs = now;
  return true;
}

// One channel-activity-detection probe, without spinning for it.
//
// The blocking scanChannel() this replaces polled the interrupt GPIO with
// yield() until the chip answered. This task is priority 5 on core 1, the
// highest there, so yield() returned immediately and the poll was a hot loop at
// 240 MHz for the whole scan — 13-18 ms per probe at SF8, and on a busy channel
// up to eighty probes per packet, about a second of full-speed CPU spent
// deciding not to transmit yet, with the rns task on the same core waiting for
// it. The verdict arrives by interrupt, and this task already has an ISR for
// that line: startChannelScan() arms the scan, the notification wakes us,
// getChannelScanResult() reads what the chip decided, and the CPU sleeps in
// between. No new interrupt wiring: every driver's channel-scan action is the
// same hook as its packet-received action — DIO1 on an SX126x, SX128x or
// LR11x0, DIO0 on an SX127x — and begin() has already set it.
//
// Telling one notification from another is the whole difficulty here, because
// the ISR is shared between CAD-done, RxDone and TxDone and says only that the
// line moved. Two things keep a stale notification from being read as a verdict:
//
//   * before the scan is armed, whatever the receiver is already holding is
//     read out and the notification counter is emptied — sendFrame() empties
//     the same counter before its own wait, for the same reason. Reading before
//     flushing is not optional: startChannelScan() clears the chip's whole
//     interrupt status, so a frame that had arrived but not been collected
//     would lose its RxDone flag to the scan and its notification to the flush,
//     and simply vanish. (The old order asked after the scan, by which time the
//     scan had already wiped the flag it was asking about.)
//   * a wake is not a verdict until the chip's own flags agree. Anything that
//     arrives without a CAD-done bit set is ignored and the wait resumes on
//     what is left of the deadline, so neither a stale notification nor a late
//     TxDone can be mistaken for a free channel.
//   * and in the other direction, the scan's own notification never leaves this
//     function. The wait consumes it — waitIrq() clears the interrupt bit,
//     and repeats of it collapse into that one bit — and the line falls when
//     startReceive() clears the flags at the bottom, which raises nothing
//     further on a rising-edge ISR. That direction is the expensive one, and it
//     has been paid for once already: the blocking scan drove the line itself
//     and left a notification behind that csmaWait()'s DIFS wait and contention
//     countdown could not tell from an incoming frame. Every probe looked like
//     traffic, the countdown declared the channel disturbed and reset to zero,
//     and the node deferred to itself for the whole of CSMA_MAX_WAIT_MS — about
//     1200 pointless wake-ups before every transmission.
//
// The deadline is the channel's, from Airtime, and reaching it means BUSY. A
// scan that never reported is a chip that is not answering, and no driver can
// tell us that: SX127x::getChannelScanResult() reads "nothing detected" off a
// scan that has not finished and calls the channel free. Deferring is the only
// safe reading of a medium nobody measured — a node that transmitted on an
// unconfirmed channel would be exactly the collision CSMA exists to avoid. It
// cannot spin, either: the wait above and the caller's CSMA_CAD_RETRY_MS pause
// between probes are both blocking waits, so a chip that never answers costs
// two sleeps per probe rather than any CPU.
//
// Listening resumes on every path out of here, and that is load-bearing twice
// over. csmaWait()'s DIFS wait and contention countdown run immediately after
// this returns, and "any traffic during either wait restarts the whole thing"
// only holds if the chip is actually receiving during them — otherwise a probe
// would leave it in standby, deaf to a frame that starts a symbol later. And
// the paths that return early for a restart leave the chip in a state
// enterSleep() can put under, which a part left mid-scan is not.
bool LoRaRadio::mediumFree() {
  const uint32_t rxDone = rxDoneFlag();
  if (rxDone && (_radio->getIrqFlags() & rxDone)) handleRadioIrq();
  flushIrq();                            // ...and only then flush

  const int16_t armed = _radio->startChannelScan();
  if (armed != RADIOLIB_ERR_NONE) {
    // Nothing was armed, so nothing is going to answer. Busy, for the same
    // reason a timeout is busy, and the chip goes back to listening. The code
    // is the driver's own and names a specific failure, so it is carried into
    // the log rather than discarded: this path used to return in silence.
    g_stats.loraCadArmErrors++;
    if (cadWarnDue(g_stats.loraCadArmErrors))
      log_w("CAD could not be armed, code %d — treating the channel as busy (%lu so far)",
            (int)armed, (unsigned long)g_stats.loraCadArmErrors);
    _radio->startReceive();
    return false;
  }

  // The wait, in slices of one CSMA slot so a restart is noticed at the same
  // granularity as everywhere else in csmaWait(). The notification ends it
  // first in every ordinary case — a scan is a handful of symbols and a slot is
  // twelve — so the slices cost nothing when the radio is healthy.
  const uint32_t cadDone  = cadDoneFlag();
  const uint32_t slice    = _airtime.slotMs();
  const uint32_t deadline = millis() + _airtime.cadTimeoutMs();
  bool completed = false, abandoned = false;
  for (;;) {
    if (_sleepRequest) { abandoned = true; break; }
    const int32_t left = (int32_t)(deadline - millis());
    if (left <= 0) break;
    const uint32_t wait = ((uint32_t)left < slice) ? (uint32_t)left : slice;
    const bool woke = waitIrq(wait);
    // A chip with no CAD bits known to this driver cannot confirm anything, so
    // there the notification has to stand for the verdict; all four parts this
    // firmware detects have them, so the fallback is unreachable today.
    //
    // Unreachable is not the same as harmless, and the take's return is what
    // makes the difference. Discarding it and accepting the fallback after the
    // first slice accepted a slice that simply expired as a finished scan, and
    // completed is what unlocks getChannelScanResult() — which on an SX127x
    // reads "nothing detected" off a scan still running and calls the channel
    // free. That is a transmission onto an unmeasured medium, the one outcome
    // this whole function exists to prevent. handleRadioIrq() reads rxDone == 0
    // as "cannot tell, do not gate"; the same reading here is "cannot tell, so
    // only a wake is evidence", never the clock.
    if (cadDone ? (_radio->getIrqFlags() & cadDone) != 0 : woke) { completed = true; break; }
  }

  // Read the verdict before anything clears the flags it is read from. Named
  // clear rather than free: a local called free shadows ::free inside a
  // translation unit that allocates.
  const bool clear = completed && _radio->getChannelScanResult() == RADIOLIB_CHANNEL_FREE;
  if (!completed) {
    // Cancel a scan that may still be running. startReceive() begins with a
    // standby on every driver, so this is the same command it would issue — but
    // cancelling is this line's job, not a side effect of the next one's.
    _radio->standby();
    // A restart is not a fault: the wait was cut short deliberately and nothing
    // was measured because nothing was waited for. Only the genuine deadline
    // counts.
    if (!abandoned) {
      g_stats.loraCadTimeouts++;
      if (cadWarnDue(g_stats.loraCadTimeouts))
        log_w("CAD did not report within %u ms — treating the channel as busy (%lu so far)",
              (unsigned)_airtime.cadTimeoutMs(), (unsigned long)g_stats.loraCadTimeouts);
    }
  }
  _radio->startReceive();
  return clear;
}

// CSMA as RNode does it: wait for the medium to be free, hold it free for a
// DIFS, then count down a randomly chosen contention window. Any traffic
// during either wait restarts the whole thing, so a node that has just heard
// a packet defers to whoever is mid-exchange. The window is drawn from a band
// selected by recent channel use, which spreads nodes out as the channel
// fills instead of having them all pile in after the same fixed backoff.
//
// Every wait in here also watches for a restart — the flag between waits rather
// than during one, so the worst case is a single wait of one slot or one retry
// interval, tens of milliseconds. This function is bounded at CSMA_MAX_WAIT_MS
// plus one probe, because the loop test is at the top: a probe entered at
// 4999 ms still gets its whole deadline, so the true ceiling is
// CSMA_MAX_WAIT_MS + Airtime::CAD_TIMEOUT_MAX_MS, about 9 s on the slowest
// channel the settings accept. The watchdog is unaffected either way — it is
// fed once per pass of this loop, at most 4050 ms apart, against a 30 s
// timeout. The restart's own wait for the radio is 250 ms, so a deferral that
// ran its full length would be many times the budget it is being held against;
// the caller checks the same flag and drops the packet.
void LoRaRadio::csmaWait() {
  const uint32_t slot = _airtime.slotMs();
  const uint32_t difs = _airtime.difsMs();
  uint8_t cwMin = 0, cwMax = Airtime::CW_PER_BAND - 1;
  const float shortTerm = _airtime.shortTermUtil(millis());
  _airtime.contentionWindow(shortTerm, cwMin, cwMax);

  const uint32_t target = (uint32_t)(cwMin + (esp_random() % (uint32_t)(cwMax - cwMin + 1))) * slot;
  const uint32_t started = millis();
  uint32_t waited = 0;                   // contention time accumulated so far

  while (!_sleepRequest && millis() - started < CSMA_MAX_WAIT_MS) {
    // Deferring to a busy channel is progress too, and this loop can hold the
    // task for CSMA_MAX_WAIT_MS on its own before a byte is sent.
    Watchdog::feed();
    if (!mediumFree()) {                 // someone is transmitting: start over
      waited = 0;
      // Checked before the pause, like the two waits below check before theirs.
      // A busy channel spends nearly all of its deferral right here, and this
      // used to be the one wait in the function that a restart could not cut
      // short: the loop test above only comes round again after the full retry
      // interval, and a probe on a slow channel takes longer than the interval
      // does.
      if (_sleepRequest) break;
      if (waitIrq(CSMA_CAD_RETRY_MS)) handleRadioIrq();
      continue;
    }

    // DIFS: the channel must stay quiet for two slots before we even start
    // counting down. A frame arriving here means it was not really idle.
    const uint32_t difsStart = millis();
    bool disturbed = false;
    while (!_sleepRequest && millis() - difsStart < difs) {
      if (waitIrq(slot)) { handleRadioIrq(); disturbed = true; break; }
    }
    if (disturbed) { waited = 0; continue; }

    // Contention window, one slot at a time so an incoming frame can pause it.
    while (!_sleepRequest && waited < target) {
      if (waitIrq(slot)) { handleRadioIrq(); disturbed = true; break; }
      waited += slot;
      if (millis() - started >= CSMA_MAX_WAIT_MS) break;
    }
    if (disturbed) { waited = 0; continue; }
    return;                              // channel held quiet: transmit — or a
                                         // restart cut the wait short, and
                                         // transmitPacket() reads the same flag
  }
  if (_sleepRequest) return;             // no "gave up" is owed for a restart
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
