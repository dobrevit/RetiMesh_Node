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
  // the allowance ranges from 0.1 % to 10 % depending on where you are, which
  // is why the limit is carried in per-mille rather than whole percent.
  struct Band {
    float       lowMhz;
    float       highMhz;
    uint16_t    permille;     // 1 = 0.1 %, 10 = 1 %, 100 = 10 %
    const char* name;
  };

  // The band containing this frequency, or nullptr when it falls outside the
  // plan we know — in which case the local rules are the operator's to apply.
  static const Band* bandFor(float freqMhz);

  // What the node will actually hold itself to: the band's allowance less a
  // safety margin, tightened further by a manual cap when one is set.
  // manualPct 0 means "whatever the band allows". Returns 0 for "no limit",
  // which happens only outside the known plan with no manual cap.
  static uint16_t effectivePermille(float freqMhz, uint8_t manualPct);

  void  addTx(uint32_t nowMs, float airMs);   // record a transmission
  float shortTermUtil(uint32_t nowMs);        // 0..1 over the last two bins
  float longTermUtil(uint32_t nowMs);         // 0..1 over the hour
  // All three take the limit in per-mille (0 = no limit), as returned by
  // effectivePermille().
  float budgetUsed(uint32_t nowMs, uint16_t limitPermille);   // 0..1+ of the allowance
  bool  locked(uint32_t nowMs, uint16_t limitPermille);
  // Seconds until the hourly figure falls back under the limit, 0 when free.
  uint32_t retryAfterS(uint32_t nowMs, uint16_t limitPermille);

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
