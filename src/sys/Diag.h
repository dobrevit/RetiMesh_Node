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

// How the previous run's restart went, in milliseconds between its steps:
// entering the restart, handing over to the core's persist-restart (the
// composite device only), and this boot. `known` is false where no restart
// preceded this boot, or the RTC domain did not hold.
struct LastRestart { uint32_t toPersistMs = 0, toBootMs = 0; bool known = false; };

// Why the previous run ended. The reset register is cleared by a power cycle,
// so "power-on" on a cold start is the expected answer, not a missing one.
struct Boot {
  uint8_t     reason          = 0;          // esp_reset_reason_t
  const char* reasonName      = "unknown";
  bool        clean           = true;       // false for panic, watchdog, brownout
  uint32_t    count           = 0;          // boots recorded in NVS, this one included
  bool        prevUptimeKnown = false;      // false after a power cut or brownout
  uint32_t    prevUptimeS     = 0;          // how long the run that just ended lasted
  LastRestart lastRestart;
};

// Call first in setup(), before anything that might itself crash.
void begin();
const Boot& boot();

// The marks a restart leaves for the next boot: on the RTC clock, in the one
// RTC-resident record beside the run length, so one magic decides whether
// RTC memory held. Bootloader stamps them as the restart goes (zero: not
// stamped); begin() reads them into boot().lastRestart and clears them.
struct RestartMarks { uint32_t entryMs, persistMs; };
RestartMarks& restartMarks();
// The RTC clock in milliseconds: it runs on through a software reset and the
// ROM session, which millis() does not.
uint32_t rtcMs();

// Starts a task and says so when it cannot. Every task this firmware creates
// goes through here, because an unchecked xTaskCreate is a node that runs
// without one of its parts and reports itself healthy: a Heltec Wireless
// Stick, whose 16 KB "rns" stack would not fit once Wi-Fi and the web server
// had taken theirs, answered "transport: online" for as long as anyone cared
// to ask while no task was driving Reticulum at all. Returns false when the
// task was not created, having logged the name, the stack it asked for and
// what the heap had left.
bool startTask(TaskFunction_t fn, const char* name, uint32_t stackBytes,
               void* arg, UBaseType_t priority, BaseType_t core);

// Keeps the current run length in RTC memory so the next boot can report it.
// Call this every pass of the main loop, not on the heartbeat: the value is
// only as accurate as its cadence, and a restart loop has to be visible as
// runs of a few seconds rather than as a string of zeroes. One word of RTC
// RAM per call and no flash wear.
void tick(uint32_t uptimeS);

const char* resetReasonName(uint8_t reason);

// Internal RAM comes in two kinds and only one of them can hold a task stack,
// a buffer or anything else addressed a byte at a time: part of what
// MALLOC_CAP_INTERNAL counts is 32-bit-only IRAM. A board can therefore report
// tens of KB free and fail to place an 8 KB stack, which is exactly what a
// Heltec Wireless Stick did — 48.9 KB free internal, 6.6 KB of it usable, and
// no task driving Reticulum as a result. So the byte-addressable figures are
// reported beside the others: they are the ones that decide.
struct Heap {
  uint32_t freeInternal;
  uint32_t minFreeInternal;                 // low-water mark since boot
  uint32_t largestBlock;                    // largest single allocation still possible
  uint32_t freeDram;                        // 8-bit-capable: what a stack or buffer can actually use
  uint32_t minFreeDram;                     // its low-water mark since boot
  uint32_t largestDramBlock;                // the largest stack this node could still place
  uint32_t freePsram;
};
Heap heap();

// What each subsystem cost to build, in the RAM that decides. costStart()
// takes the origin and every cost() after it names what has just been built
// and logs what it took, where DRAM now stands and the bill so far.
//
// This exists because the alternative is estimating, and estimating is how a
// Heltec Wireless Stick came to be paying about sixteen kilobytes for a PPP
// link whose switch was off — nobody had a per-board figure for what any of
// it cost, so nobody looked there. A boot log that carries the bill turns
// "which switch is worth making lazy" into something read rather than
// guessed, and it differs enough between boards that one board's answer is
// not another's. Costs nothing after boot: two heap queries per subsystem,
// all of them before the node is up.
void costStart();
void cost(const char* what);

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
