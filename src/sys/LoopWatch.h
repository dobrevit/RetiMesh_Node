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
//    A task cannot report its own hang. Whatever notices has to run elsewhere.
//    The first watch was kept from Reticulum's loop — until a stall took that
//    task down with the loop task, and the watch went quiet exactly when it
//    was wanted. Both are watched from the radio task now, which has outlived
//    every stall so far.
//
//    The network outlives the console. A node in this state still answers
//    HTTP, so the phase is carried in /api/status as well as in the log —
//    which means an operator can read what a stuck node is stuck in without
//    being anywhere near it, over the very link that still works.
//
//  Two tasks are watched: the Arduino loop, which carries the console, and the
//  Reticulum task, which drains the ring a client's packets arrive in. A node
//  with the second one stopped accepts nothing and looks switched off from the
//  other end while its radio still logs happily.
//
//  The cost is a pointer store and one millis() per labelled call. The
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

// Built only into a debug image. The instrument costs a pointer store and a
// millis() per labelled call, which is small but not nothing on a node whose
// whole loop is a few hundred microseconds — and a release build should carry
// no diagnostic it is not using. Off unless asked for:
//
//     PLATFORMIO_BUILD_FLAGS=-DRETIMESH_DEBUG pio run -e heltec-wp
//
// With it off every label, every pass and every check compiles to nothing, and
// the readers answer "not built". The rule itself — stallDue(), recovered() —
// is always compiled, because it is pure, it costs nothing to have, and its
// tests are worth running on every build.
#ifndef RETIMESH_DEBUG
  #define RETIMESH_DEBUG 0
#endif

namespace LoopWatch {

// The tasks worth watching: the ones that stop without saying so and take
// something visible with them when they do. The Arduino loop carries the
// console; the Reticulum task carries the transport and drains the ring a
// client's packets arrive in — a node with that one stopped accepts nothing
// and looks, from the other end, exactly like a node that is switched off.
enum Task : uint8_t { Loop = 0, Rns = 1, TaskCount = 2 };

inline const char* taskName(Task t) {
  return t == Loop ? "loop" : t == Rns ? "rns" : "?";
}

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

// --- the instrument, in a debug build only ----------------------------------
#if RETIMESH_DEBUG

// Entered a call. The string must outlive the call, which for a literal it
// does; nothing is copied.
void enter(Task task, const char* phase);

// Finished a pass.
void pass(Task task);

// --- everybody else's side --------------------------------------------------

State state(Task task);

// How long since that task finished a pass, and how long it has been in the
// call it is in. Both saturate rather than wrap.
uint32_t sincePass(Task task, uint32_t now);
uint32_t inPhase(Task task, uint32_t now);

// Called from a task that is not the one being watched, so that a stall is
// reported by something still running. Says which call it is stuck in and for
// how long, and says when it comes back.
//
// Both are checked from the radio task. It is the one that has kept running
// through every stall seen so far — and in the ones that mattered the
// Reticulum task went down with the loop task, so a watch kept there would
// have gone quiet exactly when it was needed. If the radio task is ever the
// one that stops, /api/status still carries both phases: the figures are
// stored by the watched tasks themselves and need no watcher to be read.
void check(Task task, uint32_t now);

// Names the phase in one word for a status line, never null.
const char* phaseName(Task task);

#else   // not a debug build: nothing is recorded and nothing is reported

inline void enter(Task, const char*) {}
inline void pass(Task) {}
inline void check(Task, uint32_t) {}
inline State state(Task) { return State{}; }
inline uint32_t sincePass(Task, uint32_t) { return 0; }
inline uint32_t inPhase(Task, uint32_t) { return 0; }
inline const char* phaseName(Task) { return "not built"; }

#endif

} // namespace LoopWatch

// Each watched task marks the calls it makes. Macros so a name is written once
// and reads as a label rather than as a statement.
#if RETIMESH_DEBUG
  #define LOOP_PHASE(name) LoopWatch::enter(LoopWatch::Loop, name)
  #define RNS_PHASE(name)  LoopWatch::enter(LoopWatch::Rns,  name)
#else
  #define LOOP_PHASE(name) ((void)0)
  #define RNS_PHASE(name)  ((void)0)
#endif
