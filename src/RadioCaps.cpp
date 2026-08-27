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

#include "RadioCaps.h"

#include <math.h>
#include <stdio.h>

namespace RadioCaps {

// SX127x and SX126x share this set. 500 kHz is the widest either offers.
const float kBwSubGhz[] = {
  7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f, 0.0f
};

// The SX128x has four, and they are nothing like the sub-GHz steps. A channel
// plan copied across from 868 MHz will not name a bandwidth this chip has.
const float kBwSx128x[] = { 203.125f, 406.25f, 812.5f, 1625.0f, 0.0f };

// Spreading factors stop at 7 on every part, and not for want of silicon. SF6
// switches an SX127x into implicit-header mode, where getPacketLength() returns
// a fixed configured length instead of the real one — which breaks the
// variable-length framing this firmware is built on, and would leave Airtime
// computing time-on-air for a header it is no longer sending. SF5 and SF6 on
// the SX126x and SX128x keep the explicit header but are not interoperable
// with RNode's channel set, which is the point of matching it.
// Everything either family offers, for the no-radio state. A validator that
// only knew the sub-GHz steps there would reject every valid 2.4 GHz setting
// while the operator was trying to configure their way out of a failed probe.
const float kBwAny[] = {
  7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 203.125f, 250.0f,
  406.25f, 500.0f, 812.5f, 1625.0f, 0.0f
};

const Caps kSX1276 = {
  "SX1276", 137.0f, 1020.0f, kBwSubGhz, 7, 12, 2, 17
};
const Caps kSX1262 = {
  "SX1262", 150.0f, 960.0f,  kBwSubGhz, 7, 12, 2, 22
};
// -18 to +12.5 dBm on the datasheet; RadioLib takes whole dBm and accepts 13
// as its top step. A board carrying an external PA raises the ceiling; see
// RF_TX_DBM_MAX in the board header.
const Caps kSX1280 = {
  "SX1280", 2400.0f, 2500.0f, kBwSx128x, 7, 12, -18, 13
};
// No radio answered. Nothing can be transmitted, so the validator has nothing
// to protect and stays out of the way rather than rejecting a setting the
// operator is entering ahead of fixing the wiring.
const Caps kUnknown = {
  "none", 100.0f, 2600.0f, kBwAny, 7, 12, -18, 22
};

bool channelUsable(const Caps& c, float freqMhz, float bwKhz, uint8_t sf) {
  return freqMhz >= c.freqMinMhz && freqMhz <= c.freqMaxMhz &&
         bandwidthSupported(c, bwKhz) &&
         sf >= c.sfMin && sf <= c.sfMax;
}

bool bandwidthSupported(const Caps& c, float khz) {
  // RadioLib matches to within 0.001 kHz and refuses anything else. A looser
  // tolerance here lets a near-miss through validation and into NVS, where it
  // fails to apply — and then fails again at the next boot, leaving the node
  // with its radio offline for a value the API said was fine.
  for (const float* b = c.bandwidthsKhz; *b != 0.0f; b++)
    if (fabsf(*b - khz) <= 0.001f) return true;
  return false;
}

const char* bandwidthList(const Caps& c, char* out, size_t len) {
  size_t off = 0;
  out[0] = 0;
  for (const float* b = c.bandwidthsKhz; *b != 0.0f && off < len; b++) {
    // %g so 125 prints as "125" and 203.125 keeps its decimals
    off += snprintf(out + off, len - off, "%s%g", off ? ", " : "", (double)*b);
  }
  return out;
}

} // namespace RadioCaps
