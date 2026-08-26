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
  static const uint8_t  BAND_1_MAX_PCT = 7;    // <= 7 % channel use stays in band 1
  static const uint8_t  BAND_N_MIN_PCT = 85;   // >= 85 % is the top band

  void configure(const Params& p);
  const Params& params() const { return _p; }

  float symbolTimeMs() const;
  // LoRa time on air for a payload, per the Semtech modem datasheets
  // (SX1276 6.1.4 / SX1262 6.1.4): preamble plus the symbols the payload
  // needs after coding, with the low-data-rate optimisation where it applies.
  float timeOnAirMs(size_t payloadBytes) const;

  void  addTx(uint32_t nowMs, float airMs);   // record a transmission
  float shortTermUtil(uint32_t nowMs);        // 0..1 over the last two bins
  float longTermUtil(uint32_t nowMs);         // 0..1 over the hour
  float budgetUsed(uint32_t nowMs, uint8_t limitPct);   // 0..1+ of the allowance
  bool  locked(uint32_t nowMs, uint8_t limitPct);       // limitPct 0 = no limit
  // Seconds until the hourly figure falls back under the limit, 0 when free.
  uint32_t retryAfterS(uint32_t nowMs, uint8_t limitPct);

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
