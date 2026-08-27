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
//  Diag.h — why the node restarted, and what it is running out of
//
//  A node in the field has no serial console, and without an SD card no log
//  survives a reboot. The chip does keep one thing across a reset, though: the
//  reason for it. Reading that at boot, next to a persistent boot count and
//  the length of the run that just ended, turns "it rebooted at some point"
//  into "it panicked after 4 h 12 m, and that was the third restart today" —
//  which is the difference between a soak test and a shrug.
//
//  The run length comes from RTC memory, which survives a panic, a watchdog
//  reset and a software restart but not a power cut or a brownout. Losing it
//  is therefore evidence in its own right: the rail went away rather than the
//  firmware falling over.
//
//  The rest is what a week on the bench needs to watch. Free heap, and the
//  largest block still available — fragmentation shows up in the gap between
//  those two long before an allocation actually fails. Stack headroom per
//  task, because a task that overflows takes the whole node with it and the
//  panic names it only if someone is watching the console. And the sizes of
//  the Reticulum tables, which are what grow with traffic.
// ============================================================================

#pragma once

#include <Arduino.h>
#include "Config.h"

namespace Diag {

// Why the previous run ended. The reset register is cleared by a power cycle,
// so "power-on" on a cold start is the expected answer, not a missing one.
struct Boot {
  uint8_t     reason          = 0;          // esp_reset_reason_t
  const char* reasonName      = "unknown";
  bool        clean           = true;       // false for panic, watchdog, brownout
  uint32_t    count           = 0;          // boots recorded in NVS, this one included
  bool        prevUptimeKnown = false;      // false after a power cut or brownout
  uint32_t    prevUptimeS     = 0;          // how long the run that just ended lasted
};

// Call first in setup(), before anything that might itself crash.
void begin();
const Boot& boot();

// Keeps the current run length in RTC memory so the next boot can report it.
// Call this every pass of the main loop, not on the heartbeat: the value is
// only as accurate as its cadence, and a restart loop has to be visible as
// runs of a few seconds rather than as a string of zeroes. One word of RTC
// RAM per call and no flash wear.
void tick(uint32_t uptimeS);

const char* resetReasonName(uint8_t reason);

struct Heap {
  uint32_t freeInternal;
  uint32_t minFreeInternal;                 // low-water mark since boot
  uint32_t largestBlock;                    // largest single allocation still possible
  uint32_t freePsram;
};
Heap heap();

// Bytes of stack never used, per task. `present` is false for a task this
// build did not create (no GNSS receiver, no SD card) or that has exited.
struct TaskStack { const char* name; uint32_t headroom; bool present; };
size_t taskCount();
size_t stacks(TaskStack* out, size_t max);

// The smallest headroom of any running task, and which task it belongs to.
// A value approaching zero names the task about to trip the stack canary, so
// `name` is set to nullptr — not to a placeholder — when no task was found:
// zero headroom and no reading at all must not look the same.
uint32_t lowestHeadroom(const char** name);

// Logs the heartbeat's diagnostic lines and warns when headroom or heap has
// fallen past the thresholds in Config.h. Returns true if anything warned.
// Logging only — tick() owns the run length.
bool report();

} // namespace Diag
