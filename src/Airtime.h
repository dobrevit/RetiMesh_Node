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

  uint32_t slotMs() const;
  uint32_t difsMs() const { return 2 * slotMs(); }
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
