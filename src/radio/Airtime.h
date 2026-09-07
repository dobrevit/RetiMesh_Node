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
//  Airtime.h — how much of the channel we are using, and may still use
//
//  Two jobs, both driven by the same rolling record of transmitted airtime:
//
//    * the duty-cycle limiter. ETSI gives the EU 868 MHz sub-bands an hourly
//      transmit budget (1 % for 868.0-868.6, 10 % for 869.4-869.65), and
//      exceeding it is the operator's problem, not the modem's. Airtime is
//      accumulated into 60 one-minute bins covering the last hour; when the
//      hourly figure reaches the configured limit the radio stops taking new
//      packets off the queue until the window slides.
//
//    * CSMA contention. RNode sizes its contention window from recent channel
//      use, so a busy channel spreads transmissions further apart. The same
//      bins give the short-term figure that selects the window band.
//
//  No Arduino dependencies and no internal clock — the caller passes the
//  time in — so the whole thing runs under the native test environment.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

class Airtime {
public:
  struct Params {
    uint8_t  sf             = 8;
    float    bwKhz          = 125.0f;
    uint8_t  cr             = 5;        // 5..8 => 4/5 .. 4/8
    uint16_t preambleSyms   = 18;
    bool     crcOn          = true;
    bool     implicitHeader = false;
  };

  // One-hour window in 60 bins of one minute.
  static const uint16_t BINS   = 60;
  static const uint32_t BIN_MS = 60000UL;

  // CSMA, following RNode: a slot is 12 symbols clamped to 24..100 ms, DIFS
  // is two slots, and the contention window has four bands of 15 slots.
  static const uint8_t  SLOT_SYMBOLS   = 12;
  static const uint32_t SLOT_MIN_MS    = 24;
  static const uint32_t SLOT_MAX_MS    = 100;
  static const uint8_t  CW_BANDS       = 4;
  static const uint8_t  CW_PER_BAND    = 15;
  // Stop a little short of the legal ceiling: airtime is accounted per frame
  // after the fact, and a node that aims exactly at the limit will cross it.
  // In basis points 95 % of an allowance is exact for every figure in the EU
  // plan except 0.1 %, which rounds down to 0.09 %.
  static const uint16_t DUTY_MARGIN_PCT = 5;   // hold to 95 % of the allowance

  static const uint8_t  BAND_1_MAX_PCT = 7;    // <= 7 % channel use stays in band 1
  static const uint8_t  BAND_N_MIN_PCT = 85;   // >= 85 % is the top band

  void configure(const Params& p);
  const Params& params() const { return _p; }

  float symbolTimeMs() const;
  // LoRa time on air for a payload, per the Semtech modem datasheets
  // (SX1276 6.1.4 / SX1262 6.1.4): preamble plus the symbols the payload
  // needs after coding, with the low-data-rate optimisation where it applies.
  float timeOnAirMs(size_t payloadBytes) const;

  // ---- Regulatory bands ---------------------------------------------------
  // The transmit budget is not ours to choose: it belongs to the sub-band the
  // channel sits in. In the EU 863-870 MHz SRD plan (ERC 70-03 / EN 300 220)
  // the allowance ranges from 0.1 % to 10 %, so limits are carried in basis
  // points — hundredths of a percent — which expresses every figure in the
  // plan exactly and leaves room for the safety margin to mean what it says.
  struct Band {
    float       lowMhz;
    float       highMhz;
    uint16_t    basisPoints;  // 10 = 0.1 %, 100 = 1 %, 1000 = 10 %
    const char* name;
    bool        allocated;    // false for the ranges between the sub-bands
  };

  // Which rulebook a channel falls under. This is a property of the band, not
  // of the radio: the same SX1262 is a duty-cycle device at 868 MHz and a
  // dwell-limited one at 915 MHz.
  //
  // The three are not variations on one theme — they constrain different
  // things, which is why one "duty cycle percent" setting cannot express them:
  //
  //   EuSrd868  hourly duty cycle, per sub-band, 0.1 % to 10 %. Long
  //             transmissions are fine; their total over an hour is not.
  //   UsIsm915  no hourly budget at all. FCC 15.247 instead caps how long a
  //             single transmission may sit on one channel — 400 ms for a
  //             hopping system — and a node that does not hop has to keep
  //             each packet under that or use a wide enough channel to
  //             qualify as a digital transmission system (>= 500 kHz).
  //             The binding constraint is per-packet, not per-hour.
  //   Ism2400   neither. Bounded by radiated power and by listen-before-talk,
  //             so CSMA carries the load and no budget applies.
  //
  // Saying "no limit" for the US would be wrong in the other direction: there
  // is a limit, it is just not the kind the hourly accounting can express.
  enum class Regime : uint8_t { None = 0, EuSrd868, UsIsm915, Ism2400 };

  // A region is what the operator picks; the regime is what follows from it.
  // Choosing the region first is the only honest order: "868.1 MHz" is a legal
  // channel in Europe and an illegal one in the US, and a form that offers
  // every frequency the chip can tune invites exactly that mistake. Custom is
  // kept for people who know what they are doing and are outside these three.
  enum class Region : uint8_t { Custom = 0, Eu868, Us915, Ism2400 };

  struct RegionInfo {
    Region      id;
    const char* key;          // stable identifier for the API and NVS
    const char* name;         // shown to a human
    float       lowMhz;       // the band this region may use
    float       highMhz;
    Regime      regime;
    float       defaultMhz;   // a sane channel inside it
    float       defaultBwKhz;
    uint8_t     defaultSf;
  };

  static const RegionInfo* regions(size_t& count);
  static const RegionInfo* regionByKey(const char* key);
  static const RegionInfo* regionById(Region id);
  // The region a frequency falls in, for migrating nodes configured before
  // the setting existed.
  static const RegionInfo* regionForFreq(float freqMhz);

  // The region a node is actually operating under: the one it has stored, and
  // the frequency only as a fallback for a configuration written before the
  // setting existed. Never null.
  //
  // This exists because three places needed it and derived it separately —
  // the radio when it configures the budget, the radio again when it refreshes
  // the figures, and the API when it reports what governs. One of the three was
  // missed, so the settings page told an operator their "custom" region applied
  // no plan while the other two had already stopped enforcing one. A fourth
  // caller should not be able to disagree with the first three.
  static const RegionInfo* regionFor(const char* key, float freqMhz);

  static Regime regimeFor(float freqMhz);
  static const char* regimeName(Regime r);

  // Longest a single transmission may occupy one channel, in milliseconds.
  // 0 means the regime does not constrain individual transmissions. Only
  // UsIsm915 returns non-zero today.
  static uint32_t maxDwellMs(Regime r);

  // The narrowest channel that qualifies as a digital transmission system
  // where that distinction exists, in kHz; 0 where it does not apply. A US
  // channel at or above this is not subject to the dwell limit.
  static float dtsMinBandwidthKhz(Regime r);

  // EU only: the band a channel of `bwKhz` centred on `freqMhz` must obey. A channel
  // that fits inside one sub-band gets that sub-band's allowance; one that
  // straddles a boundary gets the strictest of the bands it touches, because
  // energy lands in all of them. nullptr means the channel is outside the plan
  // entirely, where the local rules are the operator's to apply.
  static const Band* bandFor(float freqMhz, float bwKhz = 0.0f);

  // The most permissive sub-band the channel touches. Paired with bandFor()
  // it answers "what is this channel losing by straddling a boundary?".
  static const Band* mostGenerousOverlapping(float freqMhz, float bwKhz);

  // What the node will actually hold itself to: the band's allowance less a
  // safety margin, tightened further by a manual cap when one is set.
  // manualPct 0 means "whatever the band allows". Returns 0 for "no limit",
  // which happens only outside the known plan with no manual cap.
  static uint16_t effectiveBasisPoints(float freqMhz, float bwKhz, uint8_t manualPct);
  // The same, for a node whose region is known. Only EuSrd868 has a band
  // allowance to look up; every other regime leaves the manual cap as the only
  // budget, so a channel at 868 MHz under "custom" is not quietly held to the
  // European duty cycle the operator was told did not apply.
  static uint16_t effectiveBasisPoints(Regime regime, float freqMhz, float bwKhz,
                                       uint8_t manualPct);

  void  addTx(uint32_t nowMs, float airMs);   // record a transmission
  float shortTermUtil(uint32_t nowMs);        // 0..1 over the last two bins
  float longTermUtil(uint32_t nowMs);         // 0..1 over the hour
  // All three take the limit in basis points (0 = no limit), as returned by
  // effectiveBasisPoints().
  float budgetUsed(uint32_t nowMs, uint16_t limitBp);         // 0..1+ of the allowance
  bool  locked(uint32_t nowMs, uint16_t limitBp);
  // Seconds until the hourly figure falls back under the limit, 0 when free.
  uint32_t retryAfterS(uint32_t nowMs, uint16_t limitBp);

  // ---- Duty-cycled receive ------------------------------------------------
  // Whether telling an SX1262 to sleep between preamble samples would actually
  // save anything on this channel. The driver decides that silently — the
  // sleep is a function of the preamble and the symbol time, and when it comes
  // out shorter than the chip's own wake-up transition RadioLib abandons the
  // whole idea and arms a plain continuous receive instead, returning success
  // either way. A node that reported the setting rather than this answer would
  // claim a saving it is not making, which is the whole reason this lives here
  // and not inside the radio: it is pure arithmetic, it is host-testable, and
  // one caller cannot disagree with another about it.
  //
  // The constants mirror RadioLib 7.7.1 — PhysicalLayer::calculateRxDutyCycle,
  // SX126x::startReceiveDutyCycleAuto and SX126x::startReceiveDutyCycle, plus
  // SX126x::setTCXO's default. They are named so that a driver update that
  // retunes any of them shows up as a diff here and a failing test rather than
  // as a node that quietly stopped sleeping.

  // Symbols of preamble the receiver must catch to latch onto it (SX1262
  // datasheet 6.1.1.1): 8 for SF7-12, 12 for SF5-6. This firmware's floor is
  // SF7 on every chip (RadioCaps), so 8 is the figure in play.
  static const uint16_t RX_DC_MIN_SYMBOLS_SF7 = 8;
  static const uint16_t RX_DC_MIN_SYMBOLS_SF6 = 12;
  // Shutdown and startup around each wake, added to the TCXO ramp. Below that
  // total the driver does not sleep at all.
  static const uint32_t RX_DC_TRANSITION_US   = 1016;
  // ...and the amount the driver then *deducts* from the sleep before it
  // programs the chip (SX126x.cpp:479-480). Deliberately not the same number as
  // the 1016 above: the driver writes them as two separate literals, 1016 in
  // the threshold it compares against and 1000 in the subtraction it performs,
  // and folding them into one constant here would misreport the boundary in
  // whichever direction the fold went. Both are mirrored, both are named.
  static const uint32_t RX_DC_COMPENSATION_US = 1000;
  // The sleep period reaches the chip as a 24-bit count of 15.625 us ticks
  // (SetRxDutyCycle takes three bytes), so the driver rejects anything that
  // does not fit. Held as the raw ceiling rather than a microsecond figure
  // because the truncating divide by 125/8 is what decides the boundary.
  //
  // SX126x, and only the SX126x. The tick is a property of that part, not of
  // the idea: an LR11x0 counts its sleep in 30.517 us periods off the 32.768
  // kHz RTC (LR11x0::startReceiveDutyCycle), so the same 24 bits reach about
  // 512 s there against roughly 262 s here, and a slow channel this predicate
  // calls too long is one that part would have taken. Erring in that direction
  // is safe — the caller is told not to ask, and a receiver left continuously
  // on hears everything — but only while the one radio RadioCaps marks
  // rxDutyCycle is the one this arithmetic models. test_radio_plan pins that
  // pairing, so a capability bit flipped for another part fails a host test
  // instead of quietly running these numbers against the wrong clock.
  static const uint32_t RX_DC_PERIOD_RAW_MAX  = 0x00FFFFFFUL;
  // RadioLib's setTCXO() default ramp. Nothing in this firmware passes a delay,
  // so every board that names a TCXO voltage gets this one; a board with none
  // leaves the driver's delay at zero, which is why callers pass it in.
  static const uint32_t RX_DC_TCXO_DELAY_US   = 5000;

  // 0 selects the driver's own default for the spreading factor.
  static uint16_t rxDutyCycleMinSymbols(uint8_t sf);
  // Microseconds the receiver would spend asleep in each cycle. 0 means it
  // would never sleep: a preamble no longer than the two sampling windows
  // leaves nothing between them.
  static uint32_t rxDutyCycleSleepUs(uint8_t sf, float bwKhz, uint16_t preambleSyms,
                                     uint16_t minSymbols = 0);
  // ...and whether the driver will actually take that sleep. True means one
  // thing only, and it is the thing a caller may act on: the call
  //
  //     startReceiveDutyCycleAuto(<the same preambleSyms passed above>, 0)
  //
  // will arm a duty-cycled receive with it. Both arguments are part of that
  // claim, because PhysicalLayer::calculateRxDutyCycle reads both and this
  // predicate mirrors only one shape of the call:
  //
  //   * the first is the *sender's* preamble — the shortest preamble the
  //     senders we mean to hear will transmit — and it is what the sleep is
  //     computed from. It is emphatically not this node's own preamble
  //     setting; who chooses it is RadioRxArm::sizingPreamble(), and whatever
  //     it chooses has to be the figure handed to rxDutyCycleSleepUs() here as
  //     well, or the prediction is about a different sleep than the chip is
  //     given. Zero would make the driver fall back to the configured
  //     preamble, which on a node set above the floor is a longer window than
  //     any sender guarantees to fill — which is why the caller passes the
  //     figure out loud instead of leaving it to the default. Anything longer than the
  //     configured preamble is refused with
  //     RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH before any of this arithmetic
  //     runs, and a true here says nothing whatever about that call.
  //   * the second is a minSymbols override. Zero selects the driver's own
  //     default for the spreading factor, which is what rxDutyCycleMinSymbols()
  //     mirrors; pass a figure there and the same figure has to be passed to
  //     rxDutyCycleSleepUs(), or the predicate is answering about a different
  //     sleep than the one the chip will be given.
  //
  // False covers two outcomes that look nothing alike on the bench and must
  // not be told apart here, because in both of them the honest answer for a
  // caller is "do not ask the driver for this mode":
  //
  //   * too short — the sleep is under the wake-up transition, so the driver
  //     quietly arms a plain continuous receive and the node hears normally;
  //   * too long — the period will not fit the 24 bits the chip's SetRxDutyCycle
  //     command has, so the driver returns RADIOLIB_ERR_INVALID_SLEEP_PERIOD.
  //     That return is *before* the stageMode() call, so it arms nothing at
  //     all: not duty-cycled receive, not continuous receive. The chip is left
  //     in standby and the node is deaf until something else re-arms it.
  //
  // The second is reachable for any caller that sizes its window on a long
  // preamble — SF12 at 7.8 kHz has a 525 ms symbol, so 516 symbols is enough,
  // and the validator accepts up to 1000 — which is why this predicate mirrors
  // the driver's whole acceptance condition rather than only its first gate.
  // The radio no longer gets there, because it sizes on the 18-symbol interop
  // floor and so never asks for a sleep past about a second; the gate stays
  // because the predicate is a statement about the driver, not about today's
  // one caller, and because the floor is a constant somebody may raise.
  static bool rxDutyCycleEngages(uint32_t sleepUs,
                                 uint32_t tcxoDelayUs = RX_DC_TCXO_DELAY_US);

  uint32_t slotMs() const;
  uint32_t difsMs() const { return 2 * slotMs(); }

  // ---- Channel activity detection -----------------------------------------
  // How long to give one CAD probe before giving up on it, in milliseconds.
  //
  // The probe belongs to the chip: the driver puts the part into CAD for a
  // fixed number of symbols and the part raises its interrupt line when it has
  // finished. How many symbols is the driver's choice per part, and RadioLib
  // 7.7.1 picks a different one for each — 4 on the SX126x, 8 on the SX128x, 2
  // on the LR11x0, and the SX127x's own hardware-timed scan of roughly one
  // symbol. So the bound is written against the longest of them with the same
  // margin doubled over it: sixteen symbol times, plus a fixed allowance for
  // the standby transition either side of the scan, the TCXO ramp, the SPI
  // traffic that carries it and a scheduling tick or two.
  //
  // A deadline, not a duration. Nothing here predicts how long a probe takes;
  // it says when a probe has stopped being a probe and become a chip that is
  // not answering — a wedged part, or an interrupt line that is not the one the
  // board header names. It is sized to be unreachable in ordinary operation so
  // that reaching it means something, and the caller reads it as a busy channel
  // because deferring on a medium nobody measured is the only safe direction.
  //
  // Here rather than in the radio for the reason slotMs() is here: it is
  // arithmetic on the channel, it is the same arithmetic for every part, and a
  // host test can pin it where a bench session could not.
  static const uint16_t CAD_SYMBOLS     = 16;   // twice the longest driver scan
  static const uint32_t CAD_OVERHEAD_MS = 20;   // transitions, TCXO ramp, SPI, ticks
  // Floor: on the fastest channels the symbols vanish and the overhead is all
  // there is, and a deadline shorter than one scheduling round-trip would fire
  // on a healthy chip.
  static const uint32_t CAD_TIMEOUT_MIN_MS = 25;
  // Ceiling: the slowest channel this firmware will accept is SF12 at 7.8 kHz,
  // where one symbol is 525 ms and sixteen of them are 8.4 s — longer than the
  // whole CSMA deferral it would be part of, and long enough to matter to the
  // watchdog. The real scan on that channel is the driver's 4 symbols, about
  // 2.1 s, so this still clears the longest probe any reachable channel can
  // produce while staying inside both of those budgets. LoRaRadio.cpp asserts
  // the two relationships against the constants that hold them.
  static const uint32_t CAD_TIMEOUT_MAX_MS = 4000;

  uint32_t cadTimeoutMs() const;

  uint8_t  cwBand(float shortTerm) const;     // 1..CW_BANDS
  void     contentionWindow(float shortTerm, uint8_t& cwMin, uint8_t& cwMax) const;

  void reset();

private:
  void rollover(uint32_t nowMs);              // clear bins the clock skipped
  uint16_t binOf(uint32_t nowMs) const { return (uint16_t)((nowMs / BIN_MS) % BINS); }

  Params   _p;
  float    _bins[BINS] = {0};                 // milliseconds of airtime per bin
  uint32_t _lastBinStamp = 0;                 // nowMs/BIN_MS when last touched
  bool     _started = false;
};
