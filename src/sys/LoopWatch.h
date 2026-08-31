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
//  LoopWatch.h — which call the loop task is in, and who notices when it stops
//
//  A node was found with its loop task stopped and everything else running:
//  the radio task still logging beacons, Reticulum still routing, the web
//  server still answering, a phone still attached to the Reticulum port —
//  and the console dead, because the console is served from the loop task.
//  Over the cable the node looked dead. It was not; one task was blocked.
//
//  Nothing said which call. The heartbeat that would have said is printed by
//  the same task, so the moment it could have told us anything is the moment
//  it stopped being able to. That is the gap this closes, and it turns on two
//  observations:
//
//    The loop task cannot report its own hang. Whatever notices has to run on
//    another task. Reticulum's loop is the one that kept running in every
//    instance seen so far, so the watch is checked from there.
//
//    The network outlives the console. A node in this state still answers
//    HTTP, so the phase is carried in /api/status as well as in the log —
//    which means an operator can read what a stuck node is stuck in without
//    being anywhere near it, over the very link that still works.
//
//  The cost is a pointer store and one millis() per call in loop(). The
//  strings are literals, compared and printed by address; nothing is copied
//  and nothing is allocated, because a diagnostic that allocates is one that
//  stops working exactly when the node is in trouble.
//
//  Single writer, many readers, no lock. The loop task alone writes; readers
//  take a copy of two words. A reader can catch the phase pointer from one
//  pass and the timestamp from the next, which makes an age wrong by one
//  pass — microseconds when the node is healthy, and irrelevant when it is
//  stuck, which is the case this exists for.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace LoopWatch {

// Long enough that a slow flash write or a settings save is not news, short
// enough that the operator hears about a stall while it is still happening.
static const uint32_t kStallWarnMs = 5000;
// While it stays stuck, say so again this often rather than once and never.
static const uint32_t kStallRepeatMs = 30000;

// What the loop task is doing, and when it last finished a pass.
struct State {
  const char* phase = "";      // the call it entered last
  uint32_t    phaseMs = 0;     // millis() when it entered
  uint32_t    lastPassMs = 0;  // millis() at the end of the last complete pass
  uint32_t    passes = 0;      // complete passes since boot
};

// Whether a stall is worth saying out loud now. Kept apart from the clock and
// the log so the rule can be argued with on a host: an alarm that cries too
// early is turned off, and one that says nothing twice is missed.
//
// `sinceMs` is how long since the last complete pass; `lastWarnMs` is when
// this said yes last, or 0 if never.
inline bool stallDue(uint32_t sinceMs, uint32_t sinceLastWarnMs, bool warned) {
  if (sinceMs < kStallWarnMs) return false;
  if (!warned) return true;
  return sinceLastWarnMs >= kStallRepeatMs;
}

// Whether the task has come back after a stall was reported. Worth its own
// line: a stall that ended tells an operator the node recovered by itself,
// which is a different problem from one that never moves again.
inline bool recovered(bool warned, uint32_t sinceMs) {
  return warned && sinceMs < kStallWarnMs;
}

// --- the loop task's side ---------------------------------------------------

// Entered a call. The string must outlive the call, which for a literal it
// does; nothing is copied.
void enter(const char* phase);

// Finished a pass.
void pass();

// --- everybody else's side --------------------------------------------------

State state();

// How long since the loop task finished a pass, and how long it has been in
// the call it is in. Both saturate rather than wrap.
uint32_t sincePass(uint32_t now);
uint32_t inPhase(uint32_t now);

// Called from a task that is not the loop task — the Reticulum loop — so that
// a stall is reported by something that is still running. Says which call it
// is stuck in and for how long, and says when it comes back.
void check(uint32_t now);

// Names the phase in one word for a status line, never null.
const char* phaseName();

} // namespace LoopWatch

// The loop task marks each call it makes. A macro so the name is written once
// and reads as a label rather than as a statement.
#define LOOP_PHASE(name) LoopWatch::enter(name)
