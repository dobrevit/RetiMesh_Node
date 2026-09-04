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
//  RadioCaps.h — what the transceiver in this node can actually do
//
//  Until the SX1280 arrived every radio this firmware drove was a sub-GHz
//  part, so the limits were written into the places that needed them: the
//  settings validator knew 137-1020 MHz, the bandwidth list was the SX127x
//  set, the duty-cycle limiter assumed the EU 863-870 SRD plan. None of that
//  is true of a 2.4 GHz radio, and none of it was in one place.
//
//  So the radio now describes itself, and everything downstream asks. The
//  validator rejects a channel the chip cannot tune, the web UI offers the
//  bandwidths the chip actually has, and the duty-cycle limiter applies the
//  SRD plan only on the band the plan is about.
//
//  Note what is NOT here: which rules apply. That is a property of the band a
//  channel sits in, not of the chip — the same SX1262 is a 0.1 %-duty-cycle
//  device at 868 MHz and a dwell-time-limited one at 915 MHz. Airtime::Regime
//  owns that, keyed on frequency. What is actually legal where the node is
//  standing remains the operator's to know.
// ============================================================================

#pragma once

#include <Arduino.h>

namespace RadioCaps {

struct Caps {
  const char*  name;             // "SX1276", "SX1262", "SX1280", "LR1110"
  float        freqMinMhz;
  float        freqMaxMhz;
  const float* bandwidthsKhz;    // ascending, terminated by a 0.0f entry
  uint8_t      sfMin, sfMax;
  int8_t       txMinDbm, txMaxDbm;
};

// The three radios this firmware drives. Frequency spans are the datasheet
// tuning ranges, not the licence-exempt slices inside them — the validator's
// job is to reject what the chip cannot do. Both sub-GHz parts reach the US
// 902-928 MHz band as well as the EU 863-870 one; which rules apply there is
// Airtime's business, not the chip's.
extern const float kBwSubGhz[];   // SX127x / SX126x: 7.8 .. 500 kHz
extern const float kBwSx128x[];   // SX128x: 203.125 .. 1625 kHz, four steps only
extern const float kBwLr11x0[];   // LR11x0 below 1 GHz: 62.5 .. 500 kHz, four steps
extern const float kBwAny[];      // all of them, for the state where no radio answered

extern const Caps kSX1276;
extern const Caps kSX1262;
extern const Caps kSX1280;
extern const Caps kLR1110;
extern const Caps kUnknown;       // no radio detected: permissive, nothing to protect

// True when `khz` is one of the bandwidths the radio offers. Compared with a
// tolerance because the values are floats carried through JSON and NVS.
bool bandwidthSupported(const Caps& c, float khz);

// Renders the supported bandwidths as "7.8, 10.4, ... 500" for an error
// message. Returns `out` for convenience.
const char* bandwidthList(const Caps& c, char* out, size_t len);

// Can this radio be set to this channel at all? Used to decide whether a
// stored configuration survives a change of transceiver: flashing a 2.4 GHz
// image over a board that was running a sub-GHz one leaves NVS holding a
// frequency AND a bandwidth the new chip rejects, and passing either to
// begin() fails the probe — which then reports a wiring fault for what is a
// settings problem.
bool channelUsable(const Caps& c, float freqMhz, float bwKhz, uint8_t sf);

} // namespace RadioCaps
