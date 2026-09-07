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
//  RadioParkPolicy.h — how long the radio task is allowed to park
//
//  The radio task ends every pass by waiting: an arriving frame wakes it
//  through the ISR, a producer wakes it through LoRaRadio::wake(), and failing
//  both it comes round on a timer for the things nobody signals. How long that
//  wait may be is a decision with three inputs and no kernel state, so it is
//  written here as one function that returns milliseconds, rather than three
//  conditions spread through the park.
//
//  Milliseconds and not ticks on purpose: nothing in this header knows what a
//  TickType_t is, so the rule is a host test (test/test_radio_park) instead of
//  a bench session with a stopwatch. The caller converts.
//
//  The three inputs, and what each of them is protecting:
//
//   * `flagsPending` — a sleep for a restart, or a settings apply, has been
//     asked for. Both are served at the top of the next pass, so the wait is
//     zero and the pass turns over at once. This wins over everything else,
//     duty lock included: the restart budget for the radio is 250 ms
//     (Bootloader.cpp) and a park that held a shutdown for a tenth of a second
//     would spend most of it. It cannot spin, either, and that is the whole
//     difference from the ring below — the flag is consumed unconditionally at
//     the top of the pass this zero returns to, so the skip happens once,
//     whereas a queued packet under a duty lock stays queued for as long as the
//     lock does.
//
//   * `dutyLocked` — the hourly transmit budget is spent. Then the queue is not
//     going to move, and a park that skipped itself because something is in it
//     would spin this task at priority 5 for the rest of the hour: on a solar
//     node that is the difference between an idle core and a core at full tilt,
//     and nothing in the soak rig watches loop rate. So a duty-locked pass with
//     work waiting still parks for the full interval. This is the case the
//     tests exist for.
//
//   * `queued` — how many items are in the TX ring, from the caller's own read
//     of it. Anything in there and the wait is zero: the next pass takes one
//     item and transmits it, and back-to-back packets should not be paced by
//     the idle timer. (The caller may not have asked the ring at all under a
//     duty lock, where the answer cannot change the outcome; passing zero there
//     is correct rather than a shortcut.)
//
//  The result is always either 0 or idleMs. A park is never made longer than
//  the interval the watchdog and the beacon clock are sized against.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace RadioPark {

// How long the idle park may block, in milliseconds. Pure: same inputs, same
// answer, no clock and no kernel.
inline uint32_t waitMs(bool dutyLocked, size_t queued, bool flagsPending, uint32_t idleMs) {
  if (flagsPending) return 0;            // a restart or an apply, before anything else
  if (dutyLocked)   return idleMs;       // nothing will drain the ring: do not spin on it
  return queued ? 0u : idleMs;           // more to send, or nothing to do
}

} // namespace RadioPark
