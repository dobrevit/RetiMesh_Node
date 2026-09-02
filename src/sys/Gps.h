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
//  Gps.h — the u-blox receiver on boards that carry one
//
//  A hundred lines of NMEA is cheaper than a dependency: sentences are
//  assembled, checksum-verified and matched by type rather than by talker, so
//  GP, GN, GL and GA all parse. Only two are needed — RMC for position, speed
//  and, uniquely, the date; GGA for fix quality, satellite count, HDOP and
//  altitude.
//
//  The reason to have it is not really the position. A LoRa node has no
//  real-time clock, so after every reboot its idea of "now" restarts at zero
//  — which is why microStore had to be taught not to throw away records that
//  look like they come from the future. A receiver with a fix carries an
//  atomic clock's time, and adopting it gives the node a real one: log lines,
//  the SD event log and anything later that timestamps a message all become
//  meaningful across restarts.
//
//  The receiver hangs off a switched PMU rail. Boards that have one start it
//  at boot — the clock is worth more than the tens of milliamps — and the
//  settings page powers it down for a node running on a cell. On an AXP2101
//  the backup rail stays powered either way, so the receiver holds its almanac
//  and warm-starts in seconds rather than cold-starting in minutes.
// ============================================================================
#pragma once

#include "Config.h"

namespace Gps {

struct Fix {
  bool     enabled   = false;
  bool     valid     = false;      // a fix the receiver still stands behind
  uint8_t  quality   = 0;          // GGA fix quality
  uint8_t  satellites = 0;
  double   latitude  = 0.0;
  double   longitude = 0.0;
  float    altitude  = 0.0f;       // metres
  float    hdop      = 0.0f;
  float    speedKmh  = 0.0f;
  uint32_t sentences = 0;          // parsed since the receiver was switched on
  uint32_t ageMs     = 0;          // since the last sentence
  bool     timeValid = false;      // date and time seen
  bool     clockSet  = false;      // system clock adopted from the receiver
  char     utc[20]   = "";         // "YYYY-MM-DD HH:MM:SS"
};

// One tracked space vehicle, as the GSV sentences tell it. talker is the
// constellation's two letters (GP, GL, GA, GB...).
struct Sv { char talker[3]; uint8_t id; uint8_t cn0; uint32_t seenMs; };
// The SVs heard in the last ten seconds — the sky view's data. Returns how
// many were written.
size_t skyView(Sv* out, size_t max);

// Powers the receiver (via the PMU) and opens its UART, or shuts both down.
// Safe to call from any task: it holds the same lock as the reader.
void setEnabled(bool on);
bool enabled();

// Starts the reader task. Safe to call on boards without a receiver.
void begin();

Fix fix();

} // namespace Gps
