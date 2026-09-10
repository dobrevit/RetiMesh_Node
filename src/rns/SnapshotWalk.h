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
//  SnapshotWalk.h — one bounded position of the path-table walk
//
//  refreshSnapshots() in RnsTransport.cpp walks the microStore-backed path
//  table for two unrelated reasons at once: to collect the rows /api/status,
//  the OLED and the LVGL pages render, and to find entries whose receiving
//  interface has gone away so they can be dropped. What to do at any one
//  position — read it for a row, read it for the sweep alone, step past it, or
//  stop the pass — is decided here.
//
//  It is a header of its own because RnsTransport.cpp cannot be compiled for
//  the host: [env:native] has no framework, no microReticulum, no LittleFS and
//  no FreeRTOS. This is the same seam RingDrain.h uses — a pure rule with no
//  dependency of any kind, called by the firmware and asserted directly by
//  test/test_snapshot_walk.
//
//  Why the rule needed extracting at all
//  -------------------------------------
//  The budget below used to be consulted only once the rows were full — the
//  check was written `if (!wantRow && elapsed >= budget)`. "The budget ends the
//  sweep, never the rows" was the stated rule, justified on the rows being
//  bounded by count: there are at most SNAPSHOT_MAX_PATHS of them.
//
//  Bounded by count is not bounded by time. A row costs whatever the
//  filesystem charges for a record, and worse, `wantRow` is false only once a
//  row has actually been *filled* — an entry whose interface has gone away is
//  read off the filesystem and fills no row, so on a table of exactly the dead
//  entries the sweep exists to remove, `wantRow` stayed true to the end of the
//  table and no position was budgeted at all. Martin Dobrev measured the row
//  half alone at 7.5 s on a T-Beam holding 87 paths, on a node that "rebooted
//  every few minutes with the watchdog naming loopTask" (2026-09-06, on the
//  abandoned branch fix/snapshot-walk-starves-loop).
//
//  That walk runs on the RNS task: core 1, priority 3 (main.cpp). Arduino's
//  loopTask is core 1, priority 1, and it is the task setup() subscribes to the
//  watchdog. A priority-3 task that never blocks does not let a priority-1 task
//  on the same core run at all, and Watchdog::feed() is esp_task_wdt_reset(),
//  which the IDF header documents as resetting the watchdog "on behalf of the
//  currently running task" and requires "each subscribed task" to call for
//  itself — there is no call that feeds another task's subscription. So the
//  feeds in the walk answered for the RNS task and did nothing for the task
//  that was actually being starved — which is why the watchdog named loopTask
//  and not "rns".
//
//  Hence the two rules here: one budget that covers every position, and a
//  cadence on which the walk gives the core up rather than merely reporting.
// ============================================================================

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Rns {

// How long one pass of the walk may spend reading path records back off the
// filesystem.
//
// Not a tuning knob — a bound this walk did not have. Every record examined is
// one file opened, sought, read and closed, and on LittleFS that is a directory
// lookup plus several 512-byte reads, each of which disables the flash cache
// and stalls the other core while it runs. The sweep dereferenced the whole
// table in one pass, so its cost grew with the table, and on a node whose table
// had grown large enough the pass outlasted the 30-second watchdog. The node
// then rebooted every ninety seconds for two days without once saying why,
// because the watchdog's own report died on the way out (see Watchdog.h).
// Nothing here is allowed to be unbounded again.
//
// 400 ms is picked to be obviously survivable rather than optimal: two orders
// of magnitude inside the watchdog, and small next to the five seconds between
// passes (SNAPSHOT_INTERVAL_MS). It is checked between records and never inside
// one, so a pass costs it plus however long the record in hand takes.
//
// It now bounds the whole walk rather than only the part after the rows are
// full. What that costs, until row collection can be resumed across passes, is
// that a table slow enough to exhaust the budget in the row half publishes the
// rows it managed rather than a full SNAPSHOT_MAX_PATHS — and, because rows
// restart at the front of the table every pass, the sweep cursor then stops
// advancing. Both are bounded and honest: pathCount() is the whole table and is
// unaffected, and a short list is a reading that is late rather than a node
// that is down.
//
// Other surfaces quote this figure as the gap between two drains of a ring —
// main.cpp's heartbeat line, Config.h beside loraRxDrainCapped, tools/soak.py's
// column notes, docs/api.md, docs/troubleshooting.md — and RingDrain.h derives
// the radio drain's frames-per-gap arithmetic from it. Retuning it means
// visiting those.
constexpr uint32_t kWalkBudgetMs = 400;

// How much walk time may pass between two yields.
//
// Elapsed time and not a record count, and this is the whole of why. A record
// costs 30 ms in the regime this firmware links — platformio.ini records a
// 200-record walk at "about 6 s" once microStore held its segment file open
// across the iteration, and 6000/200 = 30 — and it cost 93-162 ms in the
// reopen-per-record regime before that, which is microStore's own measurement
// on the same store (FileStore.h). That is a factor of five between two shapes
// of the same library, before a worn or busy filesystem is considered, so any
// count of records is a cadence that means something different on every node.
// Elapsed time means the same thing on all of them.
//
// It also rules out the obvious shortcut of reusing Sys::RingDrain::kFeedEvery,
// which is 16, as the yield cadence: 16 x 30 ms = 480 ms is longer than the
// whole 400 ms budget, so a yield on that cadence fires *zero* times in a pass
// — at 30 ms a record the budget ends the walk before the sixteenth record is
// reached. That is not a cadence, it is a yield that never happens, and it is
// driven rather than argued in test/test_snapshot_walk. kFeedEvery stays the one definition of the *feed*
// cadence and the walk still uses it for that; this is a different clock for a
// different job, which is why it is defined here and not there. RingDrain's own
// passes are bounded by count over cheap in-memory ring items and have no
// elapsed-time notion to hang a millisecond figure on.
//
// 100 ms, which is three yields to a full budget: they land at the first record
// at or past 100 ms, 200 ms and 300 ms of walk time, and a fourth would need
// 400 ms, by which point the budget has already ended the pass. So the core is
// given up several times per pass at every per-record cost this repository has
// measured — three yields at 30 ms a record, two at 162 — while a walk short
// enough to finish inside 100 ms never reaches one at all. It is bounded at the
// other end by the same arithmetic: three yields is the ceiling however cheap a
// record becomes, which is the property no count of records has.
//
// A yield is vTaskDelay(1). CONFIG_FREERTOS_HZ is 1000 on both chips this
// firmware builds for (the esp32s3 and esp32 sdkconfigs of
// framework-arduinoespressif32-libs), so one tick is 1 ms and three yields are
// at most ~3 ms of delay per pass. Those 3 ms are spent *inside* the budget,
// not added to it — the walk measures elapsed time from its own start — so the
// yields cost this pass a fraction of one record rather than lengthening it,
// and cost the rest of the RNS task's pass nothing at all.
constexpr uint32_t kWalkYieldMs = 100;

// The relation the choice above rests on. A cadence that does not fit strictly
// more than twice in a budget gives the core up at most once a pass: the second
// yield needs 2 x the interval to have elapsed, and elapsing the whole budget
// is a stop rather than a yield.
static_assert(kWalkYieldMs > 0 && kWalkYieldMs * 2 < kWalkBudgetMs,
              "the yield cadence must fire more than once inside one budget");

// What the walk should do with the position it is standing on.
enum class WalkStep : uint8_t {
  Row,           // read it: it fills a row, and is swept as well if a sweep is running
  Examine,       // read it for the sweep alone; the rows are full, so it is not rendered
  Skip,          // step past without touching the filesystem
  StopBudget,    // out of time — the pass ends here; nextSweepPos() says where
                 // the sweep picks up, which is not always here
  StopRowsFull,  // the rows are full and no sweep wants the rest of the table
};

// Everything the decision is made from. Positions are offsets into the table in
// iteration order; times are milliseconds since the walk started, so neither
// can wrap inside one pass.
struct WalkState {
  size_t   rowsCollected = 0;   // rows filled so far this pass
  size_t   rowCap        = 0;   // SNAPSHOT_MAX_PATHS
  bool     sweeping      = false;
  size_t   pos           = 0;   // where the walk is
  size_t   sweepPos      = 0;   // where the sweep is resuming from; 0 = a fresh cycle
  uint32_t elapsedMs     = 0;
  uint32_t budgetMs      = kWalkBudgetMs;
};

// One position.
//
// Order matters and each step is here for a reason:
//
//  1. "Nothing left to do" is answered before the clock, so a pass that
//     finished what it came for is never recorded as having run out of time.
//     The difference is not cosmetic: only a budget stop writes the sweep
//     cursor.
//  2. The budget is next, and so covers every position that follows —
//     including the skip below. It used to sit after the skip, where a pass
//     stepping over a long prefix could not be stopped at all, and after the
//     row decision, where it did not apply to the expensive half of the walk.
//  3. Only then the cheap step-over: past a position the rows do not want and
//     the sweep has already been through.
inline WalkStep walkStep(const WalkState& w) {
  const bool wantRow   = w.rowsCollected < w.rowCap;
  const bool wantSweep = w.sweeping && w.pos >= w.sweepPos;
  if (!wantRow && !w.sweeping) return WalkStep::StopRowsFull;
  if (w.elapsedMs >= w.budgetMs) return WalkStep::StopBudget;
  if (!wantRow && !wantSweep)   return WalkStep::Skip;
  return wantRow ? WalkStep::Row : WalkStep::Examine;
}

// Whether the walk should give the core up before touching this record.
//
// `elapsedMs` and `lastYieldMs` are both measured from the start of the walk,
// and `lastYieldMs` starts at zero — so the first yield of a pass comes at the
// first record at or past the interval, not at the first record.
inline bool walkShouldYield(uint32_t elapsedMs, uint32_t lastYieldMs,
                            uint32_t intervalMs = kWalkYieldMs) {
  return (uint32_t)(elapsedMs - lastYieldMs) >= intervalMs;
}

// Where the next sweep pass resumes, given where this one stopped.
//
// Forwards only, within a cycle. The budget can now end the walk anywhere,
// including in the row half at a position *behind* the cursor — rows are
// collected from the front of the table on every pass, so a stop at row 13 with
// the sweep at 60 is the ordinary case, not the exotic one. Writing the stop
// position in unconditionally would drag the sweep back to the front and leave
// the tail of the table never examined, which is the exact bug the resuming
// cursor was added to avoid. Reaching the end of the table (`!ranOut`) closes
// the cycle instead and puts the next one a kStaleSweepMs out.
inline size_t nextSweepPos(size_t sweepPos, size_t stoppedAt, bool ranOut) {
  if (!ranOut) return 0;
  return stoppedAt > sweepPos ? stoppedAt : sweepPos;
}

} // namespace Rns
