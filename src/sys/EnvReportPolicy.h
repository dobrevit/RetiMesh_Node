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
//  EnvReportPolicy.h — what an environmental sensor's three facts add up to
//
//  A sensor reports three things that are not the same thing: whether a part
//  is fitted, whether a conversion has ever landed, and how many sampling
//  intervals have since produced nothing. Every surface has to turn that
//  triple into one thing to say, and each one was doing the arithmetic itself
//  — the console, the mono display page, the LVGL home screen and the portal,
//  four copies of the same threshold with a fifth number in the driver.
//
//  They agreed. That is the point at which this project calls it a finding:
//  four copies that agree are four copies that will drift, and only one of
//  them will get fixed. So the ladder is written once, here, with no Arduino
//  include, and test/test_environment pins it.
//
//  Why the states are the ones they are
//  ------------------------------------
//  `valid` is not a health flag and must never be read as one. This driver
//  keeps its last conversion deliberately, because a stale temperature is
//  still the best answer available and the caller is the one who knows how
//  stale is too stale (Environment.h). So a part that answered at boot and
//  died an hour ago reports `valid: true` for ever, and the only things that
//  say otherwise are the age and the missed count. Every surface has to draw
//  the difference between:
//
//    Absent      nothing is fitted, or nothing answered at boot. A wiring
//                question, and not one that waiting will fix.
//    WarmingUp   fitted, no reading yet, and not enough intervals have passed
//                to call it a fault. The first half minute after boot.
//    NoReading   fitted, still no reading, and enough intervals have gone by
//                that "warming up" would be a promise rather than a report.
//    Stale       a reading is being shown *and* the part has stopped
//                answering. Both are true at once and both have to be said.
//    Fresh       a reading, and the part is still answering.
//
//  The threshold, and the one in the driver
//  ----------------------------------------
//  Two intervals is one minute at the shipped cadence, which is long enough to
//  stop saying "warming up" and short enough that a person watching a boot
//  does not sit through a lie. The driver's own log threshold is deliberately
//  *later* than this one (Environment.cpp, kComplainAfter) — the surfaces say
//  what is happening as soon as they can, the log complains once it is sure —
//  and the two numbers are related rather than equal, which is exactly why
//  they are named next to each other rather than both being spelled 2.
// ============================================================================
#pragma once

#include <stdint.h>

namespace EnvReportPolicy {

// Sampling intervals with no reading before a fitted part stops being "warming
// up" and starts being "not answering".
constexpr uint32_t kQuietIntervals = 2;

enum class State : uint8_t {
  Absent    = 0,
  WarmingUp = 1,
  NoReading = 2,
  Stale     = 3,
  Fresh     = 4,
};

inline State classify(bool present, bool valid, uint32_t missed) {
  if (!present) return State::Absent;
  if (!valid)   return missed >= kQuietIntervals ? State::NoReading : State::WarmingUp;
  return missed ? State::Stale : State::Fresh;
}

// The wire name for the state, so the portal can switch on it instead of
// re-deriving the ladder in JavaScript — the one copy this header could not
// otherwise reach. Stable: it is emitted in /api/status and documented.
inline const char* name(State s) {
  switch (s) {
    case State::Absent:    return "absent";
    case State::WarmingUp: return "warming_up";
    case State::NoReading: return "no_reading";
    case State::Stale:     return "stale";
    case State::Fresh:     return "fresh";
  }
  return "absent";
}

} // namespace EnvReportPolicy
