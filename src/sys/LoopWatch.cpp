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

#if RETIMESH_DEBUG
#include <Arduino.h>

namespace LoopWatch {

// One row per watched task. Written only by that task, read by anyone. Plain
// volatile words rather than a mutex: a diagnostic that can block is one more
// thing that can hang the task it is watching, which would be an unusually
// complete way to fail.
struct Slot {
  const char* volatile phase = "boot";
  volatile uint32_t    phaseMs = 0;
  volatile uint32_t    lastPass = 0;
  volatile uint32_t    passes = 0;
};
static Slot sSlot[TaskCount];

// The watcher's own state, touched only by the task that calls check().
static bool     sWarned[TaskCount]     = { false, false };
static uint32_t sWarnedAtMs[TaskCount] = { 0, 0 };

static Slot& slot(Task t) { return sSlot[t < TaskCount ? t : Loop]; }

void enter(Task task, const char* phase) {
  Slot& sl = slot(task);
  sl.phase = phase ? phase : "";
  sl.phaseMs = millis();
}

void pass(Task task) {
  Slot& sl = slot(task);
  sl.lastPass = millis();
  sl.passes++;
  sl.phase = "idle";               // between passes, so a stall names a call
}

State state(Task task) {
  Slot& sl = slot(task);
  State s;
  s.phase = (const char*)sl.phase;
  s.phaseMs = sl.phaseMs;
  s.lastPassMs = sl.lastPass;
  s.passes = sl.passes;
  return s;
}

const char* phaseName(Task task) {
  const char* p = (const char*)slot(task).phase;
  return p ? p : "";
}

// Saturating, because a reader can see a timestamp from a pass that has not
// happened yet on its own clock and would otherwise report four billion
// milliseconds of stall.
static uint32_t since(uint32_t now, uint32_t then) {
  const uint32_t d = now - then;
  return (int32_t)d < 0 ? 0 : d;
}

uint32_t sincePass(Task task, uint32_t now) { return since(now, slot(task).lastPass); }
uint32_t inPhase(Task task, uint32_t now)   { return since(now, slot(task).phaseMs); }

void check(Task task, uint32_t now) {
  // Nothing to say before the first pass: a task that has never run is not a
  // task that has stopped.
  if (!slot(task).passes) return;
  const uint32_t stuck = sincePass(task, now);

  if (recovered(sWarned[task], stuck)) {
    log_w("%s: the %s task is running again after %lu ms stopped",
          taskName(task), taskName(task), (unsigned long)since(now, sWarnedAtMs[task]));
    sWarned[task] = false;
    return;
  }
  if (!stallDue(stuck, since(now, sWarnedAtMs[task]), sWarned[task])) return;

  // Said from whichever task called this — deliberately not the one being
  // watched, which by definition cannot say it. The phase is the call it went
  // into and did not come out of, which is the whole point of the exercise.
  log_e("%s: the %s task has not completed a pass for %lu ms — stuck in \"%s\" "
        "(entered %lu ms ago, %lu passes since boot). %s",
        taskName(task), taskName(task), (unsigned long)stuck, phaseName(task),
        (unsigned long)inPhase(task, now), (unsigned long)slot(task).passes,
        task == Loop
          ? "The console is served from that task, so the cable will be silent"
          : "That task drains the ring a client's packets arrive in, so the node "
            "will accept nothing while it is stopped");
  sWarned[task] = true;
  sWarnedAtMs[task] = now;
}

} // namespace LoopWatch

#endif  // RETIMESH_DEBUG
