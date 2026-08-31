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
//  LoopWatch.cpp — the two words the loop task writes, and the watcher
// ============================================================================
#include "LoopWatch.h"
#include <Arduino.h>

namespace LoopWatch {

// Written only by the loop task, read by anyone. Plain volatile words rather
// than a mutex: a diagnostic that can block is one more thing that can hang
// the task it is watching, which would be an unusually complete way to fail.
static const char* volatile sPhase   = "boot";
static volatile uint32_t     sPhaseMs   = 0;
static volatile uint32_t     sLastPass  = 0;
static volatile uint32_t     sPasses    = 0;

// The watcher's own state, touched only by the task that calls check().
static bool     sWarned = false;
static uint32_t sWarnedAtMs = 0;

void enter(const char* phase) {
  sPhase = phase ? phase : "";
  sPhaseMs = millis();
}

void pass() {
  sLastPass = millis();
  sPasses++;
  sPhase = "idle";                 // between passes, so a stall names a call
}

State state() {
  State s;
  s.phase = (const char*)sPhase;
  s.phaseMs = sPhaseMs;
  s.lastPassMs = sLastPass;
  s.passes = sPasses;
  return s;
}

const char* phaseName() {
  const char* p = (const char*)sPhase;
  return p ? p : "";
}

// Saturating, because a reader can see a timestamp from a pass that has not
// happened yet on its own clock and would otherwise report four billion
// milliseconds of stall.
static uint32_t since(uint32_t now, uint32_t then) {
  const uint32_t d = now - then;
  return (int32_t)d < 0 ? 0 : d;
}

uint32_t sincePass(uint32_t now) { return since(now, sLastPass); }
uint32_t inPhase(uint32_t now)   { return since(now, sPhaseMs); }

void check(uint32_t now) {
  // Nothing to say before the first pass: a node in setup() has a loop task
  // that has never run, which is not a stall.
  if (!sPasses) return;
  const uint32_t stuck = sincePass(now);

  if (recovered(sWarned, stuck)) {
    log_w("loop: the loop task is running again after %lu ms stopped",
          (unsigned long)since(now, sWarnedAtMs));
    sWarned = false;
    return;
  }
  if (!stallDue(stuck, since(now, sWarnedAtMs), sWarned)) return;

  // Said from whichever task called this — deliberately not the loop task,
  // which by definition cannot say it. The phase is the call it went into and
  // did not come out of, which is the whole point of the exercise.
  log_e("loop: the loop task has not completed a pass for %lu ms — stuck in \"%s\" "
        "(entered %lu ms ago, %lu passes since boot). Everything else is still "
        "running; the console is served from that task, so the cable will be silent",
        (unsigned long)stuck, phaseName(), (unsigned long)inPhase(now),
        (unsigned long)sPasses);
  sWarned = true;
  sWarnedAtMs = now;
}

} // namespace LoopWatch
