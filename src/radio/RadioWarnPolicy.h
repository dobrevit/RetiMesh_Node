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
//  RadioWarnPolicy.h — how often a repeating radio failure may reach the log
//
//  The failures this paces are never one-offs: a chip that has stopped
//  answering fails every CAD probe, and one deferral is about fifty of them
//  (CSMA_MAX_WAIT_MS over a deadline plus CSMA_CAD_RETRY_MS), so a line per
//  probe would be fifty blocking console writes for every packet sent. A driver
//  that refuses the duty-cycled receive refuses it on every re-arm, which is at
//  least once per packet. The first failure of each kind is reported the moment
//  it happens and after that at most one line an interval from each, every one
//  carrying its running total so the rate is readable from the log alone.
//
//  Each caller brings its own stamp and its own count, which is the whole
//  reason this takes a reference rather than owning one: a CAD storm must not
//  be able to suppress the arm warning, or the other way round.
//
//  A rate-limited line is not the record of the fault, only the notice of it.
//  For carrier sense that record is cad_timeouts and cad_arm_errors on the
//  STATUS line and in /api/status; for the duty-cycled receive it is
//  rx_duty_cycle_armed, which stands false for as long as the refusals last. A
//  fault this quiet has to be visible whether or not anyone was watching the
//  log at the time.
//
//  Here rather than inside LoRaRadio for the reason RadioParkPolicy is: the
//  rule is arithmetic on numbers the caller already holds, and the only thing
//  that tied it to the firmware was the millis() it read for itself. The clock
//  comes in as an argument, so "the first failure always logs", "a repeat
//  inside the interval does not" and "the two stamps do not suppress each
//  other" are host tests (test/test_radio_warn) rather than a bench session
//  with a stopwatch and a wedged transceiver.
// ============================================================================
#pragma once

#include <stdint.h>

namespace RadioWarn {

// Whether this failure may be logged. `lastMs` is the caller's own stamp of
// when it last logged this kind of failure and is updated when the answer is
// true; `count` is the caller's running total *including* this failure, so the
// first one arrives here as 1. Pure apart from that one stamp: same inputs,
// same answer, no clock of its own.
//
// The first failure of a kind is never suppressed — count 1 returns true
// whatever the stamp says, which is what makes a fault visible the moment it
// starts rather than at the end of the first interval. After that the subtract
// is done in unsigned arithmetic on purpose: it is the same comparison
// everywhere else in the firmware makes against millis(), and it stays correct
// across the 49-day wrap.
inline bool due(uint32_t& lastMs, uint32_t count, uint32_t nowMs, uint32_t intervalMs) {
  if (count > 1 && nowMs - lastMs < intervalMs) return false;
  lastMs = nowMs;
  return true;
}

} // namespace RadioWarn
