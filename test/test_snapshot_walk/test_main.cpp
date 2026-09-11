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


// SnapshotWalk: the bound that keeps the path-table walk from starving the
// task underneath it, and the rules that make that bound safe.
//
// What is pinned here is what the fix is:
//
//   * the budget stops a pass while the rows are still filling — it used to be
//     guarded on !wantRow, so the expensive half of the walk had no bound at
//     all;
//   * it stops one on the free step-over path too, which used to `continue`
//     before the check was ever reached;
//   * "the rows are full and no sweep wants the rest" is still a clean finish
//     and not a budget stop, because only a budget stop leaves a cursor
//     part-way down the table;
//   * neither cursor ever moves backwards, which is a hazard the first of
//     those creates: a stop in the row half is at the front of the table, and
//     writing it in would drag the sweep there and leave the tail unexamined;
//   * a cursor the table has shrunk past starts the cycle again instead of
//     latching, which is the other side of "forwards only": a ratchet with
//     nothing to answer to would never come back inside the table;
//   * the budget stays well inside the interval a pass is gated on, which is
//     what actually returns the core to loopTask — the yields are a margin;
//   * and the yield cadence really does give the core up several times inside
//     one budget at the per-record costs this repository has measured — the
//     exact thing reusing Sys::RingDrain::kFeedEvery would have failed at.
//
// And what the pass that resumed row collection across passes had to hold on
// to, all of it learned from the abandoned branch that tried it first:
//
//   * row collection carries a cursor of its own, under the same two rules the
//     sweep's is under — test_a_row_bound_table_now_advances_the_cursor is the
//     limitation test that used to assert the opposite;
//   * a pass may not be stopped by the budget until it has dereferenced one
//     record, so it always reaches its own cursor and always moves it forward,
//     whatever a step comes to cost;
//   * the end of a pass is the rows being *full*, not this position wanting no
//     row — "no row here" is what a resuming pass sees at position 0, and
//     ending on it published 19 rows on a node holding 87;
//   * only a whole list is published; a budget-stopped pass keeps its prefix;
//   * a pass that threw between its last row and its cursor starts the cycle
//     again rather than collecting those rows a second time;
//   * the removal that follows the walk is on the *walk's* clock, so the two
//     together are one budget and not two.
//
// refreshSnapshots() itself cannot be built for the host — [env:native] has no
// framework, no microReticulum, no LittleFS and no FreeRTOS — so the rule was
// lifted into a pure header the firmware calls and this suite drives directly,
// the way test_ring_drain drives RingDrain.h. The walk harness below is that
// loop written out again over a table of flags and a clock: if the two shapes
// diverge, this suite stops describing the firmware, so it is kept deliberately
// close to the call site.
//
// The per-record costs the yield tests use are the repository's own, not
// invented for this suite:
//
//   30 ms   platformio.ini, lib_deps: a 200-record walk "costs about 6 s now"
//           once microStore held its segment file open across the iteration.
//           6000/200 = 30.
//   162 ms  microStore's FileStore.h, load_value(): "one record cost 93-162 ms"
//           on a 200-record path store in the reopen-per-record regime, "nearly
//           all of it under lfs_dir_find". The slow end of that range is the
//           worst per-record figure anywhere in the tree.
#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "Config.h"                       // SNAPSHOT_MAX_PATHS, SNAPSHOT_SWEEP_INTERVAL_MS
#include "../../src/sys/RingDrain.h"      // kFeedEvery, the cadence that will not do
#include "../../src/rns/SnapshotWalk.h"

namespace {

// The firmware's row cap, not a number of this suite's own: a retune of
// SNAPSHOT_MAX_PATHS must move what is asserted here with it.
constexpr size_t kRowCap = SNAPSHOT_MAX_PATHS;

// The ceiling refreshSnapshots() passes nextIntervalMs() — the minute between
// dead-path sweep cycles. Read from Config.h for the same reason as the cap:
// the firmware's kStaleSweepMs is that same figure, and this suite writing
// 60000 out for itself is how the two come to disagree after a retune.
constexpr uint32_t kSweepCeilingMs = SNAPSHOT_SWEEP_INTERVAL_MS;

// Per-record costs, sourced above.
constexpr uint32_t kFastRecordMs = 30;
constexpr uint32_t kSlowRecordMs = 162;

// ---------------------------------------------------------------------------
// One position's inputs, spelled out so a test reads as a sentence.
// ---------------------------------------------------------------------------
// `readAny` is true because that is what every position after the first of a
// pass looks like; the two tests about the forward-progress rule say otherwise
// with havingReadNothing() below.
Rns::WalkState at(size_t rows, bool sweeping, size_t pos, size_t sweepPos,
                  uint32_t elapsedMs, uint32_t budgetMs = Rns::kWalkBudgetMs) {
  Rns::WalkState w;
  w.rowsCollected = rows;
  w.rowCap        = kRowCap;
  w.rowPos        = 0;
  w.sweeping      = sweeping;
  w.pos           = pos;
  w.sweepPos      = sweepPos;
  w.elapsedMs     = elapsedMs;
  w.budgetMs      = budgetMs;
  w.readAny       = true;
  return w;
}

// The same position on a pass that is continuing a part-built row list.
Rns::WalkState resumingRowsAt(Rns::WalkState w, size_t rowPos) {
  w.rowPos = rowPos;
  return w;
}

// The same position on a pass that has not dereferenced anything yet.
Rns::WalkState havingReadNothing(Rns::WalkState w) {
  w.readAny = false;
  return w;
}

// ---------------------------------------------------------------------------
// The walk, as refreshSnapshots() runs it, over a synthetic table.
//
// `live[k]` is "position k has a receiving interface", which is what decides
// whether reading it fills a row. A dead entry costs exactly as much to read as
// a live one and fills nothing — that asymmetry is the reason the old !wantRow
// guard left the whole table unbudgeted, so the harness has to model it.
//
// The clock only moves when a record is dereferenced, plus one tick per yield:
// stepping over a position is free (test_typed_store_iterator pins that against
// microStore itself). `skipMs` exists to say what happens when it is *not* —
// the regression the forward-progress rule is a defence against — and is zero
// everywhere else.
// ---------------------------------------------------------------------------
struct Pass {
  size_t   rows      = 0;      // rows in the staging list after this pass
  size_t   added     = 0;      // ...of which this pass collected these
  size_t   reads     = 0;      // records dereferenced
  size_t   skipped   = 0;      // positions stepped over
  size_t   yields    = 0;
  size_t   stoppedAt = 0;      // the position the walk left off at
  // Row collection's own two, which are not the walk's once the sweep's share
  // of the budget begins: the walk goes on to the sweep's cursor without the
  // rows, so where it stopped is not where they did.
  size_t   rowStoppedAt = 0;   // one past the last position the rows were served at
  bool     rowsCut      = false;  // the share ended the row half before the list did
  bool     ranOut    = false;  // the budget ended it
  bool     rowsFull  = false;  // it ended because there was nothing left to do
  uint32_t elapsedMs = 0;
  std::vector<size_t> swept;   // dead entries the pass found while sweeping
  std::vector<uint32_t> yieldAt;  // walk time at each yield, before its tick is paid
};

// `costMs` is what each position's record costs, for the one property that
// needs two different per-record figures inside one pass: a filesystem charges
// what it charges, and the range measured on one store is 93-162 ms rather than
// a constant (FileStore.h). Positions past its end fall back to `recordMs`, so
// a short vector says "these first few, then the usual".
Pass runWalk(const std::vector<bool>& live, bool sweeping, size_t sweepPos,
             uint32_t recordMs, uint32_t yieldMs = Rns::kWalkYieldMs,
             uint32_t tickMs = 1, uint32_t budgetMs = Rns::kWalkBudgetMs,
             size_t rowPos = 0, size_t rowsAlready = 0, uint32_t skipMs = 0,
             const std::vector<uint32_t>* costMs = nullptr) {
  Pass r;
  r.rows = rowsAlready;                    // the cycle's list, carried in
  uint32_t lastYieldMs = 0;
  for (size_t pos = 0; pos < live.size(); pos++) {
    Rns::WalkState w =
        at(r.rows, sweeping, pos, sweepPos, r.elapsedMs, budgetMs);
    w.rowPos  = rowPos;
    w.readAny = r.reads > 0;
    const Rns::WalkStep step = Rns::walkStep(w);
    r.stoppedAt = pos;
    if (step == Rns::WalkStep::StopRowsFull) { r.rowsFull = true; return r; }
    if (step == Rns::WalkStep::StopBudget)   { r.ranOut   = true; return r; }
    // Before the step-over, as at the call site: a position the share denied
    // the rows comes back as a Skip when the sweep does not want it either.
    if (step == Rns::WalkStep::Row)   r.rowStoppedAt = pos + 1;
    else if (Rns::rowShareDenied(w))  r.rowsCut      = true;
    if (step == Rns::WalkStep::Skip)         { r.skipped++; r.elapsedMs += skipMs; continue; }

    if (Rns::walkShouldYield(w.elapsedMs, lastYieldMs, yieldMs)) {
      lastYieldMs = w.elapsedMs;
      r.yields++;
      r.yieldAt.push_back(w.elapsedMs);
      r.elapsedMs += tickMs;               // a yield is paid out of the budget
    }
    r.reads++;
    r.elapsedMs += (costMs && pos < costMs->size()) ? (*costMs)[pos] : recordMs;
    if (!live[pos]) { if (sweeping) r.swept.push_back(pos); continue; }
    if (step == Rns::WalkStep::Row) { r.rows++; r.added++; }
  }
  r.stoppedAt = live.size();               // the walk reached the end of the table
  return r;
}

std::vector<bool> table(size_t n, bool allLive) { return std::vector<bool>(n, allLive); }

// ---------------------------------------------------------------------------
// One pass with the cursor rules around it, in the order refreshSnapshots()
// applies them: resume against the table as it is now, walk, write the cursor
// back. `sweeping` is forced on, which is the firmware's state whenever a
// cursor is part-way down a table or the minute is up.
//
// Row collection is left at the front here, so this drives the sweep cursor on
// its own. The whole of refreshSnapshots()'s bookkeeping — both cursors, the
// staging list and the publish — is Node below.
// ---------------------------------------------------------------------------
struct Sweep {
  size_t cursor = 0;      // where the next pass would resume
  bool   closed = false;  // this pass reached the end of the table
  Pass   pass;
};

Sweep sweepOnce(const std::vector<bool>& live, size_t cursor, uint32_t recordMs) {
  Sweep s;
  const size_t from = Rns::resumeCursor(cursor, live.size());
  s.pass   = runWalk(live, true, from, recordMs);
  s.closed = !s.pass.ranOut;
  s.cursor = Rns::nextCursor(from, s.pass.stoppedAt, s.pass.ranOut);
  return s;
}

// ---------------------------------------------------------------------------
// Everything refreshSnapshots() carries in statics from one pass to the next,
// and the order it applies the rules in: resume both cursors against the table
// as it is now, empty the staging list only if the row cycle is starting, walk,
// write both cursors back, and publish the rows only if the cycle finished.
//
// Kept deliberately close to the call site, like runWalk() above: if the two
// shapes diverge this suite stops describing the firmware.
// ---------------------------------------------------------------------------
struct Node {
  size_t sweepPos     = 0;
  size_t rowPos       = 0;
  size_t staged       = 0;      // rows the current cycle has collected so far
  size_t published    = 0;      // rows in the list readers actually see
  bool   rowsWhole    = false;  // a cycle has been published at least once
  bool   rowPassClean = true;   // the last pass wrote its row cursor back
  size_t rowCycles    = 0;
  size_t sweepCloses  = 0;
  size_t sweptTotal   = 0;
  size_t budgetStops  = 0;
};

// `aborts` is a pass that throws mid-walk: Diag::guard catches it in the
// firmware and nothing after the walk runs — no cursor written back, no
// publish. The staging list keeps what the walk pushed into it before the
// throw, because it is the caller's own vector and the throw does not unwind
// it. That is the one state in which the list and the cursor disagree, and it
// is what Rns::resumeRowCursor() is for.
Pass onePass(Node& n, const std::vector<bool>& live, uint32_t recordMs,
             bool sweeping = true, uint32_t skipMs = 0, bool aborts = false) {
  n.sweepPos = Rns::resumeCursor(n.sweepPos, live.size());
  n.rowPos   = Rns::resumeRowCursor(n.rowPos, live.size(), n.rowPassClean);
  if (n.rowPos == 0) n.staged = 0;
  n.rowPassClean = false;        // set again when the cursor is written back
  const Pass r = runWalk(live, sweeping, n.sweepPos, recordMs, Rns::kWalkYieldMs, 1,
                         Rns::kWalkBudgetMs, n.rowPos, n.staged, skipMs);
  n.staged = r.rows;             // pushed as the walk went, so it survives a throw
  if (aborts) return r;
  n.sweptTotal += r.swept.size();
  if (r.ranOut) n.budgetStops++;
  // The row half's own two figures, and its own answer to "was it cut short":
  // the budget, or the sweep's share of the budget.
  const bool rowsShort = r.ranOut || r.rowsCut;
  const bool rowsDone  = Rns::rowCycleDone(r.rows, kRowCap, rowsShort);
  n.rowPos = rowsDone ? 0 : Rns::nextCursor(n.rowPos, r.rowStoppedAt, rowsShort);
  n.rowPassClean = true;         // list and cursor agree again
  if (sweeping) {
    n.sweepPos = Rns::nextCursor(n.sweepPos, r.stoppedAt, r.ranOut);
    if (!r.ranOut) n.sweepCloses++;
  }
  if (rowsDone) { n.published = r.rows; n.rowsWhole = true; n.rowCycles++; }
  return r;
}

// ---------------------------------------------------------------------------
// The removal that follows the walk, as refreshSnapshots() runs it.
//
// The point of the harness is the clock. The loop at the call site is bounded
// by passMayStop((uint32_t)(millis() - walkStartMs), ...) — the *walk's* start,
// deliberately, so that the walk and the removal together are one budget and
// not two. Only the primitive was pinned before this; the sharing was not, and
// swapping walkStartMs for a removeStartMs of its own would hand the removal a
// second full budget without failing anything.
//
// `removeMs` is this harness's own parameter and not a measurement: nothing in
// this tree times a BasicFileStore::remove(), which is a tombstone appended and
// flushed plus an index entry written and flushed (FileStore.h). The tests pick
// figures that divide the budget so the arithmetic is readable.
//
// `allGone` is the case where every key in `stale` has already been removed by
// something else: remove_path() returns false, and it returns before touching
// flash, so the iteration costs nothing.
// ---------------------------------------------------------------------------
struct Removal {
  size_t   dropped   = 0;      // remove_path() said yes
  size_t   attempts  = 0;      // ...out of this many tries
  uint32_t elapsedMs = 0;      // still measured from the start of the *walk*
  bool     stopped   = false;  // the budget ended it with entries left
};

Removal runRemoval(size_t stale, uint32_t walkElapsedMs, uint32_t removeMs,
                   uint32_t budgetMs = Rns::kWalkBudgetMs, bool allGone = false) {
  Removal r;
  r.elapsedMs = walkElapsedMs;          // one clock, and the walk started it
  for (size_t k = 0; k < stale; k++) {
    if (Rns::passMayStop(r.elapsedMs, r.attempts > 0, budgetMs)) { r.stopped = true; break; }
    r.attempts++;
    if (!allGone) { r.dropped++; r.elapsedMs += removeMs; }
  }
  return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The constants, and the relation the yield cadence rests on
// ---------------------------------------------------------------------------

void test_the_constants_are_what_the_fix_committed_to() {
  TEST_ASSERT_EQUAL_UINT32(400, Rns::kWalkBudgetMs);
  TEST_ASSERT_EQUAL_UINT32(100, Rns::kWalkYieldMs);
  // Strictly more than twice, not merely twice: a cadence of exactly half the
  // budget yields once and then needs the whole budget elapsed for its second,
  // which is a stop.
  TEST_ASSERT_TRUE(Rns::kWalkYieldMs * 2 < Rns::kWalkBudgetMs);
}

void test_the_budget_stays_well_inside_the_interval_a_pass_is_gated_on() {
  // The relation the fix actually rests on, and the one most likely to be
  // broken by someone buying rows back with a bigger budget. refreshSnapshots()
  // runs at most once per SNAPSHOT_INTERVAL_MS, measured from the start of a
  // pass: while a walk ends well inside that window the RNS task spends the
  // rest of it going round its loop and sleeping 10 ms at a time, and those
  // sleeps are the only core 1 the priority-1 loopTask gets. A walk that
  // outlasts the window makes the gate a no-op and the passes run back to back,
  // which is the shape that took a T-Beam down at 7.5 s a walk. The firmware
  // static_asserts this beside the call; it is asserted here too, because this
  // is the file kWalkBudgetMs would be retuned in.
  //
  // The relation, not the interval: retuning SNAPSHOT_INTERVAL_MS is allowed
  // and this suite has no opinion on the figure, only on the two holding
  // together. An equality on 5000 here would have failed such a retune while
  // saying nothing about the rule.
  TEST_ASSERT_TRUE_MESSAGE(Rns::walkBudgetFitsInterval(SNAPSHOT_INTERVAL_MS),
                           "the rule is the relation: the walk's budget must stay "
                           "inside a quarter of SNAPSHOT_INTERVAL_MS");
  // A quarter is where the line is: the same rule read the other way says the
  // budget may not exceed a quarter of the interval.
  TEST_ASSERT_TRUE(Rns::walkBudgetFitsInterval(Rns::kWalkBudgetMs * 4));
  TEST_ASSERT_FALSE(Rns::walkBudgetFitsInterval(Rns::kWalkBudgetMs * 4 - 1));
  // At the figures the firmware ships, that leaves 12.5x rather than 4x.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkBudgetMs * 12,
                                      (uint32_t)SNAPSHOT_INTERVAL_MS);
  // The rule is written `budget <= interval / 4` rather than
  // `budget * 4 <= interval`, which are the same relation on every pair of
  // integers that does not overflow and differ on the pairs that do: a
  // kWalkBudgetMs at or above 2^30 wraps the product to something small and the
  // multiplied form answers "fits". The budget is a constant, so only the
  // interval side is reachable from here — an interval too small to hold any
  // budget is refused, and is not a division by anything.
  TEST_ASSERT_FALSE(Rns::walkBudgetFitsInterval(0));
  TEST_ASSERT_FALSE(Rns::walkBudgetFitsInterval(1));
  TEST_ASSERT_FALSE(Rns::walkBudgetFitsInterval(3));
}

void test_a_budget_that_outlasts_the_interval_is_what_the_guard_is_for() {
  // The passage in SnapshotWalk.h that this pins used to work its example at
  // 3 s and call it "a walk that never stops". It is not. The guard's rule read
  // as a bound on the budget is budget <= interval/4, so a 3 s budget does fail
  // it on the shipped interval...
  TEST_ASSERT_GREATER_THAN_UINT32((uint32_t)SNAPSHOT_INTERVAL_MS / 4, 3000u);
  // ...but a 3 s pass, plus even the worst record this tree has measured, still
  // ends well before the 5 s gate reopens, so the RNS task reaches the 10 ms
  // sleep at the end of its loop a couple of hundred times in the remainder.
  // The guard is a 4x margin, not the cliff.
  TEST_ASSERT_LESS_THAN_UINT32((uint32_t)SNAPSHOT_INTERVAL_MS, 3000u + kSlowRecordMs);

  // The cliff is a budget the interval cannot hold: the pass is still walking
  // when the gate reopens, so the next one starts at once and the task never
  // gets to its sleep. That is the shape that took the T-Beam down.
  TEST_ASSERT_GREATER_THAN_UINT32((uint32_t)SNAPSHOT_INTERVAL_MS, 6000u);

  // And the yields buy nothing there, which is the whole point of the example.
  // Sixty would be 6000/100; the real figure is 49, because a yield costs a
  // tick that is itself charged to the budget, so at 30 ms a record the yields
  // land about 121 ms apart. Driven rather than divided out — dividing it out
  // is how the comment came to claim 29 for the 3 s case, which is 24.
  const Pass wide = runWalk(table(4000, true), true, 0, kFastRecordMs,
                            Rns::kWalkYieldMs, 1, 6000);
  TEST_ASSERT_TRUE(wide.ranOut);
  TEST_ASSERT_EQUAL_size_t(49, wide.yields);
  const Pass narrow = runWalk(table(4000, true), true, 0, kFastRecordMs,
                              Rns::kWalkYieldMs, 1, 3000);
  TEST_ASSERT_TRUE(narrow.ranOut);
  TEST_ASSERT_EQUAL_size_t(24, narrow.yields);
  // At the shipped budget it is three, and that is the figure the header quotes.
  const Pass shipped = runWalk(table(4000, true), true, 0, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t(3, shipped.yields);
}

// ---------------------------------------------------------------------------
// One position at a time
// ---------------------------------------------------------------------------

void test_the_budget_stops_a_pass_while_rows_are_still_filling() {
  // The defect, at its smallest. The rows are nowhere near full and no sweep is
  // running, so every earlier shape of this rule returned a row here: the check
  // was `if (!wantRow && elapsed >= budget)`.
  TEST_ASSERT_TRUE(Rns::WalkStep::Row ==
                   Rns::walkStep(at(1, false, 1, 0, Rns::kWalkBudgetMs - 1)));
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(at(1, false, 1, 0, Rns::kWalkBudgetMs)));
  // And with a sweep running as well, which is the case the old guard did cover
  // — but only after the rows had filled.
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(at(1, true, 1, 0, Rns::kWalkBudgetMs)));
}

void test_the_budget_applies_on_the_skip_path() {
  // Rows full, sweeping, and behind the cursor: the position is stepped over
  // for free while there is time...
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip ==
                   Rns::walkStep(at(kRowCap, true, 3, 10, Rns::kWalkBudgetMs - 1)));
  // ...and the pass ends there once there is not. The skip used to `continue`
  // before the budget was ever consulted, so a walk with a long prefix to step
  // over could not be stopped on that path at all.
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(at(kRowCap, true, 3, 10, Rns::kWalkBudgetMs)));
}

void test_rows_full_and_no_sweep_is_a_finish_not_a_budget_stop() {
  TEST_ASSERT_TRUE(Rns::WalkStep::StopRowsFull ==
                   Rns::walkStep(at(kRowCap, false, 9, 0, 0)));
  // Even with the budget spent, because the difference is not cosmetic: only a
  // budget stop writes the sweep cursor, and a pass that finished what it came
  // for has not run out of anything.
  TEST_ASSERT_TRUE(Rns::WalkStep::StopRowsFull ==
                   Rns::walkStep(at(kRowCap, false, 9, 0, Rns::kWalkBudgetMs * 4)));
}

void test_a_position_neither_half_wants_is_stepped_over() {
  // Rows full, sweep running, but the cursor is ahead: nothing to do here and
  // nothing to pay for it.
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip == Rns::walkStep(at(kRowCap, true, 0, 5, 0)));
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip == Rns::walkStep(at(kRowCap, true, 4, 5, 0)));
  // At the cursor it becomes the sweep's business.
  TEST_ASSERT_TRUE(Rns::WalkStep::Examine == Rns::walkStep(at(kRowCap, true, 5, 5, 0)));
  TEST_ASSERT_TRUE(Rns::WalkStep::Examine == Rns::walkStep(at(kRowCap, true, 6, 5, 0)));
}

void test_a_row_is_read_whether_or_not_the_sweep_wants_it() {
  // While the rows are unfilled the record is read regardless of the cursor:
  // it is being paid for anyway, so the sweep gets it for nothing.
  TEST_ASSERT_TRUE(Rns::WalkStep::Row == Rns::walkStep(at(0, true, 0, 40, 0)));
  TEST_ASSERT_TRUE(Rns::WalkStep::Row == Rns::walkStep(at(0, false, 0, 0, 0)));
}

// ---------------------------------------------------------------------------
// Whole passes
// ---------------------------------------------------------------------------

void test_a_slow_table_is_bounded_by_the_budget_in_the_row_half() {
  // 200 live entries at 30 ms each is 6 s of reading — the walk that tripped
  // the watchdog. The rows never fill, so the old guard never consulted the
  // budget; this one ends the pass inside it.
  const Pass r = runWalk(table(200, true), false, 0, kFastRecordMs);
  TEST_ASSERT_TRUE(r.ranOut);
  TEST_ASSERT_FALSE(r.rowsFull);
  TEST_ASSERT_LESS_THAN_size_t(kRowCap, r.rows);            // a prefix, not a full list
  // The budget is checked between records, so a pass overruns by at most the
  // record in hand plus the tick a yield costs.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkBudgetMs, r.elapsedMs);
  TEST_ASSERT_LESS_THAN_UINT32(Rns::kWalkBudgetMs + kFastRecordMs + 1 + 1, r.elapsedMs);
}

void test_a_table_of_dead_entries_is_bounded_too() {
  // The case the old guard could not stop at all: an entry with no receiving
  // interface is read off the filesystem and fills no row, so `wantRow` stayed
  // true to the end of the table and every position was dereferenced with no
  // check. This is exactly the table the sweep exists to clean up.
  const Pass r = runWalk(table(200, false), true, 0, kFastRecordMs);
  TEST_ASSERT_TRUE(r.ranOut);
  TEST_ASSERT_EQUAL_size_t(0, r.rows);
  TEST_ASSERT_LESS_THAN_size_t(200, r.reads);
  TEST_ASSERT_EQUAL_size_t(r.reads, r.swept.size());        // every one read was swept
  TEST_ASSERT_LESS_THAN_UINT32(Rns::kWalkBudgetMs + kFastRecordMs + 1 + 1, r.elapsedMs);
}

void test_a_small_table_finishes_inside_the_budget_and_closes_the_cycle() {
  // Nothing here should behave differently from before the fix: a table the
  // walk can finish is finished, the cycle closes, and no yield is reached.
  const Pass r = runWalk(table(3, true), true, 0, kFastRecordMs);
  TEST_ASSERT_FALSE(r.ranOut);
  TEST_ASSERT_EQUAL_size_t(3, r.reads);
  TEST_ASSERT_EQUAL_size_t(3, r.rows);
  TEST_ASSERT_EQUAL_size_t(0, r.yields);                    // 90 ms of walk, cadence is 100
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextCursor(0, r.stoppedAt, r.ranOut));
}

void test_a_pass_stops_on_rows_full_when_nothing_else_needs_the_rest() {
  // No sweep due: the walk collects its cap and stops, having read exactly the
  // rows and not one record more.
  const Pass r = runWalk(table(kRowCap * 3, true), false, 0, 0);
  TEST_ASSERT_TRUE(r.rowsFull);
  TEST_ASSERT_FALSE(r.ranOut);
  TEST_ASSERT_EQUAL_size_t(kRowCap, r.rows);
  TEST_ASSERT_EQUAL_size_t(kRowCap, r.reads);
  TEST_ASSERT_EQUAL_size_t(0, r.skipped);
  TEST_ASSERT_EQUAL_size_t(kRowCap, r.stoppedAt);
}

void test_a_sweeping_pass_steps_over_the_prefix_it_has_already_examined() {
  // Rows fill at the front, the cursor is past them, and the positions in
  // between cost nothing. Free records so the budget is not what ends it.
  const size_t cursor = kRowCap + 10;
  const Pass r = runWalk(table(kRowCap + 20, true), true, cursor, 0);
  TEST_ASSERT_FALSE(r.ranOut);
  TEST_ASSERT_EQUAL_size_t(kRowCap, r.rows);
  TEST_ASSERT_EQUAL_size_t(10, r.skipped);                  // kRowCap .. cursor-1
  TEST_ASSERT_EQUAL_size_t(kRowCap + 10, r.reads);          // the rows, then the tail
}

// ---------------------------------------------------------------------------
// The sweep cursor
// ---------------------------------------------------------------------------

void test_the_sweep_cursor_never_moves_backwards() {
  // The hazard the budget's new reach creates. Rows are collected from the
  // front of the table on every pass, so a budget stop in the row half is at a
  // position *behind* a cursor that has already worked its way down the table.
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextCursor(60, 13, true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextCursor(60, 0,  true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextCursor(60, 59, true));
  // Forwards it does move.
  TEST_ASSERT_EQUAL_size_t(61, Rns::nextCursor(60, 61, true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextCursor(60, 60, true));
  // And reaching the end of the table closes the cycle whatever the position.
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextCursor(60, 999, false));
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextCursor(60, 0,   false));
}

void test_a_row_bound_pass_leaves_the_cursor_where_it_was() {
  // End to end: a table slow enough that the budget runs out in the row half,
  // with a sweep already part-way down it. Written out as the firmware writes
  // it — the cursor is fed the position the walk stopped at.
  //
  // What reaches that state has narrowed, and the narrowing is the sweep's
  // share doing its job: on the shipped regime the rows now give the budget up
  // half way and the walk goes on to the cursor, so an ordinary budget stop is
  // at or *past* it (the case below, and the fairness tests further down). What
  // is left behind it is the regression the harness's `skipMs` models — a
  // microStore whose ++ loads a record, which is what the abandoned branch ran
  // on — where the pass is stopped in a step-over before it reaches anything.
  const size_t cursor = 60;
  const Pass r = runWalk(table(200, true), true, cursor, kFastRecordMs,
                         Rns::kWalkYieldMs, 1, Rns::kWalkBudgetMs,
                         /*rowPos=*/0, /*rowsAlready=*/0, /*skipMs=*/kFastRecordMs);
  TEST_ASSERT_TRUE(r.ranOut);
  TEST_ASSERT_LESS_THAN_size_t(cursor, r.stoppedAt);        // stopped behind the cursor
  TEST_ASSERT_EQUAL_size_t(cursor, Rns::nextCursor(cursor, r.stoppedAt, r.ranOut));
  // Which is not what an unconditional write would have done: that is the value
  // on the other side of the clamp, and it is at the front of the table.
  TEST_ASSERT_NOT_EQUAL_size_t(r.stoppedAt,
                               Rns::nextCursor(cursor, r.stoppedAt, r.ranOut));

  // The same table with stepping free, which is what this build links: the
  // pass reaches the cursor and the write-back moves it forward. Both halves
  // of the rule, on one table.
  const Pass now = runWalk(table(200, true), true, cursor, kFastRecordMs);
  TEST_ASSERT_TRUE(now.ranOut);
  TEST_ASSERT_GREATER_THAN_size_t(cursor, now.stoppedAt);
  TEST_ASSERT_EQUAL_size_t(now.stoppedAt,
                           Rns::nextCursor(cursor, now.stoppedAt, now.ranOut));
}

void test_a_cursor_past_the_end_of_the_table_starts_the_cycle_again() {
  // The other side of "forwards only". Positions run 0..size-1, so the size
  // itself is already past the end.
  TEST_ASSERT_EQUAL_size_t(0, Rns::resumeCursor(150, 100));
  TEST_ASSERT_EQUAL_size_t(0, Rns::resumeCursor(100, 100));
  TEST_ASSERT_EQUAL_size_t(0, Rns::resumeCursor(1, 0));      // an emptied table
  // A cursor the table still holds is left exactly where it was.
  TEST_ASSERT_EQUAL_size_t(99, Rns::resumeCursor(99, 100));
  TEST_ASSERT_EQUAL_size_t(0,  Rns::resumeCursor(0, 100));
}

void test_a_cursor_left_past_a_shrunken_table_does_not_latch() {
  // Reachable through the sweep's own behaviour: what it collects is capped at
  // SNAPSHOT_MAX_PATHS, so one pass can drop 64 entries from under a cursor
  // sitting beyond them. Here a table of 200 with the sweep at 150 is 100 long
  // by the next pass.
  const std::vector<bool> shrunk = table(100, true);
  const size_t stranded = 150;

  // The rule itself, which is what latches it: nextCursor() only moves forwards,
  // so a pass that stops anywhere below an out-of-range cursor hands that same
  // out-of-range value straight back. No position in this table is at or past
  // it, so no pass can stop at or past it either.
  TEST_ASSERT_EQUAL_size_t(stranded, Rns::nextCursor(stranded, 0, true));
  TEST_ASSERT_EQUAL_size_t(stranded, Rns::nextCursor(stranded, 99, true));

  // Unresumed and driven, on the stepping-costs regression: the pass stops in
  // the row half with the budget spent, below the cursor, and the cycle never
  // closes and the minute clock is never stamped again.
  const Pass raw = runWalk(shrunk, true, stranded, kFastRecordMs, Rns::kWalkYieldMs, 1,
                           Rns::kWalkBudgetMs, /*rowPos=*/0, /*rowsAlready=*/0,
                           /*skipMs=*/kFastRecordMs);
  TEST_ASSERT_TRUE(raw.ranOut);
  TEST_ASSERT_LESS_THAN_size_t(stranded, raw.stoppedAt);
  TEST_ASSERT_EQUAL_size_t(stranded,
                           Rns::nextCursor(stranded, raw.stoppedAt, raw.ranOut));

  // With stepping free, which is what this build links, the same unresumed
  // pass comes out differently and no better: the rows spend their share, every
  // position below the stranded cursor is stepped over for nothing, and the
  // walk falls off the end of the table having examined not one record for the
  // sweep — and calls that a finished cycle. resumeCursor() is what keeps that
  // unreachable, which is why it is asked before `sweeping` is decided.
  const Pass off = runWalk(shrunk, true, stranded, kFastRecordMs);
  TEST_ASSERT_FALSE(off.ranOut);
  TEST_ASSERT_EQUAL_size_t(shrunk.size(), off.stoppedAt);
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextCursor(stranded, off.stoppedAt, off.ranOut));

  // Resumed against the table as it is now, the cursor is inside it again and
  // this pass advances it like any other.
  const Sweep s = sweepOnce(shrunk, stranded, kFastRecordMs);
  TEST_ASSERT_GREATER_THAN_size_t(0, s.cursor);
  TEST_ASSERT_LESS_THAN_size_t(shrunk.size(), s.cursor);

  // And a table cheap enough to walk closes the cycle from the same stale
  // start, rather than being stuck outside itself for ever.
  const Sweep cheap = sweepOnce(shrunk, stranded, 0);
  TEST_ASSERT_TRUE(cheap.closed);
  TEST_ASSERT_EQUAL_size_t(0, cheap.cursor);
}

// ---------------------------------------------------------------------------
// Row collection resumes across passes, and what that closed
// ---------------------------------------------------------------------------

void test_a_row_bound_table_now_advances_the_cursor() {
  // This was test_a_row_bound_table_never_advances_the_cursor, and it was
  // written to be flipped. What it pinned was the cost of bounding the row
  // half: rows were re-collected from the front of the table on every pass, so
  // on a table slow enough to spend the budget before they filled, every pass
  // read the same live prefix and stopped in the same place. Over thirty passes
  // the cursor settled at 14 and never moved, the cycle closed zero times, not
  // one of the hundred dead entries was ever seen, and the published list was a
  // permanent fourteen-row prefix stamped fresh every five seconds.
  //
  // The same table, the same per-record cost, with row collection carrying a
  // cursor of its own. The three assertions that were `== 0` are the three that
  // matter.
  std::vector<bool> live = table(200, true);
  for (size_t k = 100; k < live.size(); k++) live[k] = false;

  Node n;
  std::vector<size_t> rowCursor;
  for (int passNo = 0; passNo < 5; passNo++) {
    onePass(n, live, kFastRecordMs);
    rowCursor.push_back(n.rowPos);
  }
  // Four budget-stopped passes walking fourteen records each, and the fifth
  // fills the cap and closes the cycle. The cursor moves every pass, which is
  // the whole of what was broken.
  TEST_ASSERT_EQUAL_size_t(14, rowCursor[0]);
  TEST_ASSERT_EQUAL_size_t(28, rowCursor[1]);
  TEST_ASSERT_EQUAL_size_t(42, rowCursor[2]);
  TEST_ASSERT_EQUAL_size_t(56, rowCursor[3]);
  TEST_ASSERT_EQUAL_size_t(0,  rowCursor[4]);   // wrapped: the cycle closed
  TEST_ASSERT_EQUAL_size_t(1, n.rowCycles);
  // And a whole list is what got published, not the fourteen-row prefix.
  TEST_ASSERT_EQUAL_size_t(kRowCap, n.published);
  TEST_ASSERT_TRUE(n.rowsWhole);

  // The dead half beyond the prefix is reached as well, which it never was.
  // The sweep cursor used to advance only on whatever was left of a budget
  // after the rows had had theirs, which on this table was 30 further passes to
  // the first dead entry and 110 to a closed sweep cycle. It gets a guaranteed
  // half of the budget now whenever taking none would starve it
  // (Rns::rowShareSpent), and the same two figures are 5 and 18 — driven,
  // beside the rest of that rule, in
  // test_the_sweep_cursor_advances_on_every_sweeping_pass below. The bounds
  // asserted here stay loose because the exact figures are an artefact of one
  // synthetic table with nothing ever removed from it; that they are finite is
  // this test's property.
  size_t firstSwept = 0, firstClose = 0;
  for (int passNo = 1; passNo <= 200; passNo++) {
    const Pass r = onePass(n, live, kFastRecordMs);
    if (!firstSwept && !r.swept.empty()) firstSwept = (size_t)passNo;
    if (!firstClose && n.sweepCloses)    firstClose = (size_t)passNo;
  }
  TEST_ASSERT_GREATER_THAN_size_t(0, firstSwept);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(60, firstSwept);
  TEST_ASSERT_GREATER_THAN_size_t(0, firstClose);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(150, firstClose);
  TEST_ASSERT_GREATER_THAN_size_t(0, n.sweptTotal);
  TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, n.sweepCloses);
}

void test_a_pass_may_not_stop_on_budget_before_it_has_read_something() {
  // The forward-progress guarantee, at one position. With the budget spent and
  // nothing read yet, a position the pass would step over is still stepped
  // over, and a position it wants is still read.
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip ==
                   Rns::walkStep(havingReadNothing(
                       resumingRowsAt(at(14, false, 0, 0, Rns::kWalkBudgetMs), 14))));
  TEST_ASSERT_TRUE(Rns::WalkStep::Row ==
                   Rns::walkStep(havingReadNothing(at(14, false, 20, 0, Rns::kWalkBudgetMs))));
  // Once one record has been dereferenced the budget bites at the next position.
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(resumingRowsAt(at(14, false, 0, 0, Rns::kWalkBudgetMs), 14)));
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(at(14, false, 20, 0, Rns::kWalkBudgetMs)));
  // The same rule the removal is bounded by, asked directly.
  TEST_ASSERT_FALSE(Rns::passMayStop(Rns::kWalkBudgetMs * 10, false));
  TEST_ASSERT_TRUE(Rns::passMayStop(Rns::kWalkBudgetMs, true));
  TEST_ASSERT_FALSE(Rns::passMayStop(Rns::kWalkBudgetMs - 1, true));
}

void test_a_pass_reaches_its_own_cursor_even_if_stepping_stops_being_free() {
  // The structural defence, driven. Stepping costs nothing today — microStore
  // loads on dereference and not on ++, which test_typed_store_iterator pins —
  // so the prefix a resuming pass steps over is free. The abandoned branch
  // fix/snapshot-walk-starves-loop tried a row cursor back when stepping *did*
  // load, and the pass stopped inside its own prefix on every pass: "pinned at
  // position 15 of 87 for ever, publishing an empty list".
  //
  // Here every step costs a whole record, so the budget is long gone before the
  // cursor at 50 is reached. The pass still gets there, still reads one record,
  // and the cursor still moves forward.
  const Pass r = runWalk(table(200, true), false, 0, kFastRecordMs, Rns::kWalkYieldMs,
                         1, Rns::kWalkBudgetMs, /*rowPos=*/50, /*rowsAlready=*/50,
                         /*skipMs=*/kFastRecordMs);
  TEST_ASSERT_TRUE(r.ranOut);
  TEST_ASSERT_EQUAL_size_t(50, r.skipped);
  TEST_ASSERT_EQUAL_size_t(1, r.reads);            // exactly one, beyond the cursor
  TEST_ASSERT_EQUAL_size_t(1, r.added);
  TEST_ASSERT_EQUAL_size_t(51, r.stoppedAt);
  TEST_ASSERT_GREATER_THAN_size_t(50, Rns::nextCursor(50, r.stoppedAt, r.ranOut));
  // The price of the guarantee: the pass costs the prefix plus one record
  // rather than the budget. That is only ever paid if the dependency regresses,
  // and it is the right way round — a bound that could stall the node for ever
  // is worse than one that can be overrun.
  TEST_ASSERT_GREATER_THAN_UINT32(Rns::kWalkBudgetMs, r.elapsedMs);
}

void test_a_resuming_pass_does_not_end_at_its_own_front_door() {
  // The second thing the abandoned branch got wrong, and it published 19 rows
  // on a node holding 87. With a row cursor, `!wantRow` is true at position 0 on
  // every resuming pass — it wants no row *there*. Ending the pass on that, and
  // treating the short list as finished, is the bug; the end condition is the
  // rows being full.
  const Rns::WalkState front = resumingRowsAt(at(14, false, 0, 0, 0), 14);
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip == Rns::walkStep(front));
  TEST_ASSERT_FALSE(Rns::WalkStep::StopRowsFull == Rns::walkStep(front));
  // Full is full, though, and that is still a clean finish.
  TEST_ASSERT_TRUE(Rns::WalkStep::StopRowsFull ==
                   Rns::walkStep(resumingRowsAt(at(kRowCap, false, 0, 0, 0), 14)));

  // End to end: a pass resuming at 14 on a table with no sweep due walks past
  // its prefix and collects the next fourteen rather than stopping at nothing.
  const Pass r = runWalk(table(200, true), false, 0, kFastRecordMs, Rns::kWalkYieldMs,
                         1, Rns::kWalkBudgetMs, /*rowPos=*/14, /*rowsAlready=*/14);
  TEST_ASSERT_EQUAL_size_t(14, r.skipped);
  TEST_ASSERT_EQUAL_size_t(14, r.added);
  TEST_ASSERT_EQUAL_size_t(28, r.rows);
  TEST_ASSERT_EQUAL_size_t(28, r.stoppedAt);
}

void test_only_a_whole_list_is_published() {
  // A budget-stopped pass has a valid prefix and nothing more. What readers see
  // stays at the last whole list rather than becoming four paths on a node
  // holding eighty.
  TEST_ASSERT_FALSE(Rns::rowCycleDone(14, kRowCap, true));   // stopped, not full
  TEST_ASSERT_TRUE(Rns::rowCycleDone(kRowCap, kRowCap, true));  // the cap filled
  TEST_ASSERT_TRUE(Rns::rowCycleDone(3, kRowCap, false));    // walked to the end
  TEST_ASSERT_TRUE(Rns::rowCycleDone(0, kRowCap, false));    // an empty table

  Node n;
  const std::vector<bool> live = table(200, true);
  // Nothing published until the fifth pass closes the cycle.
  for (int k = 0; k < 4; k++) {
    onePass(n, live, kFastRecordMs, /*sweeping=*/false);
    TEST_ASSERT_EQUAL_size_t(0, n.published);
    TEST_ASSERT_FALSE(n.rowsWhole);
    TEST_ASSERT_GREATER_THAN_size_t(0, n.staged);      // but the prefix is kept
  }
  onePass(n, live, kFastRecordMs, /*sweeping=*/false);
  TEST_ASSERT_EQUAL_size_t(kRowCap, n.published);
  TEST_ASSERT_TRUE(n.rowsWhole);
  // And a table the walk finishes in one pass publishes on that pass, which is
  // every ordinary node: no list is delayed by this.
  Node small;
  onePass(small, table(3, true), kFastRecordMs, /*sweeping=*/false);
  TEST_ASSERT_EQUAL_size_t(3, small.published);
  TEST_ASSERT_TRUE(small.rowsWhole);
  TEST_ASSERT_EQUAL_size_t(0, small.rowPos);
}

// ---------------------------------------------------------------------------
// A pass that threw, and the removal's clock
// ---------------------------------------------------------------------------

void test_a_pass_that_threw_starts_the_row_cycle_again() {
  // The newest piece of bookkeeping in refreshSnapshots() and the one with no
  // test: the flag that says whether the last pass got as far as writing its
  // row cursor back. The staging list and the cursor have to agree, and the one
  // thing that parts them is the pass throwing between the last row it pushed
  // and the write-back — Diag::guard catches that, and reading a record under
  // memory pressure can cause it.
  const std::vector<bool> live = table(200, true);
  Node n;

  // Two clean passes: fourteen rows, then twenty-eight, cursor following.
  onePass(n, live, kFastRecordMs, /*sweeping=*/false);
  TEST_ASSERT_EQUAL_size_t(14, n.rowPos);
  TEST_ASSERT_EQUAL_size_t(14, n.staged);
  TEST_ASSERT_TRUE(n.rowPassClean);

  // The third throws after the walk. Its rows are in the list — the vector is
  // the caller's own and the throw does not unwind it — and the cursor was
  // never written, so it still points at 14 with 28 rows collected.
  const Pass thrown = onePass(n, live, kFastRecordMs, /*sweeping=*/false,
                              /*skipMs=*/0, /*aborts=*/true);
  TEST_ASSERT_EQUAL_size_t(28, thrown.rows);
  TEST_ASSERT_EQUAL_size_t(28, n.staged);
  TEST_ASSERT_EQUAL_size_t(14, n.rowPos);
  TEST_ASSERT_FALSE(n.rowPassClean);

  // That disagreement is exactly what the rule is asked about. Resumed as an
  // ordinary cursor it would say 14 — and the next pass would then collect
  // positions 14..27 a second time into a list that already holds them.
  TEST_ASSERT_EQUAL_size_t(14, Rns::resumeCursor(n.rowPos, live.size()));
  TEST_ASSERT_EQUAL_size_t(0, Rns::resumeRowCursor(n.rowPos, live.size(), n.rowPassClean));

  // So the next pass starts at the front with an empty list: nothing stepped
  // over, fourteen rows collected, fourteen in the list rather than forty-two.
  const Pass again = onePass(n, live, kFastRecordMs, /*sweeping=*/false);
  TEST_ASSERT_EQUAL_size_t(0, again.skipped);
  TEST_ASSERT_EQUAL_size_t(14, again.added);
  TEST_ASSERT_EQUAL_size_t(14, again.rows);
  TEST_ASSERT_EQUAL_size_t(14, n.rowPos);
  TEST_ASSERT_TRUE(n.rowPassClean);

  // And the cycle that follows it closes on a whole list of exactly the cap,
  // not on a list padded out by the rows the throw left behind.
  for (int k = 0; k < 4; k++) onePass(n, live, kFastRecordMs, /*sweeping=*/false);
  TEST_ASSERT_EQUAL_size_t(kRowCap, n.published);
  TEST_ASSERT_EQUAL_size_t(1, n.rowCycles);

  // The rule on its own, both conditions of it. A clean pass keeps a cursor the
  // table still holds; an unclean one starts again whatever the cursor said;
  // and a cursor the table has shrunk past starts again either way.
  TEST_ASSERT_EQUAL_size_t(14, Rns::resumeRowCursor(14, 200, true));
  TEST_ASSERT_EQUAL_size_t(0,  Rns::resumeRowCursor(14, 200, false));
  TEST_ASSERT_EQUAL_size_t(0,  Rns::resumeRowCursor(150, 100, true));
  TEST_ASSERT_EQUAL_size_t(0,  Rns::resumeRowCursor(150, 100, false));
}

void test_the_removal_is_bounded_by_the_walks_clock_and_not_a_fresh_one() {
  // The removal at the end of a pass is bounded by
  // passMayStop(millis() - walkStartMs, ...) — the walk's start, so that the
  // walk and the removal together are one budget rather than two. The rule was
  // pinned; the sharing was not, and a removeStartMs of its own would be a
  // silent second budget.
  //
  // 20 ms a removal is this harness's own figure and divides the budget by
  // twenty, so what each case spends is readable rather than asserted blind.
  const uint32_t kRemoveMs = 20;

  // A pass whose walk spent the whole budget still removes one entry. Not
  // none: passMayStop() may not stop a pass that has done nothing, which is
  // the same forward-progress rule the walk is under.
  const Removal spent = runRemoval(kRowCap, Rns::kWalkBudgetMs, kRemoveMs);
  TEST_ASSERT_EQUAL_size_t(1, spent.attempts);
  TEST_ASSERT_EQUAL_size_t(1, spent.dropped);
  TEST_ASSERT_TRUE(spent.stopped);

  // A pass whose walk finished early removes as many as the rest of the budget
  // holds: 400 / 20.
  const Removal early = runRemoval(kRowCap, 0, kRemoveMs);
  TEST_ASSERT_EQUAL_size_t(20, early.dropped);
  TEST_ASSERT_TRUE(early.stopped);

  // And the sharing itself, which is the assertion a fresh clock would fail.
  // The same removal after a walk that used 300 of the 400 gets what is left,
  // five entries — where a clock of its own would give it the full twenty
  // again and let one pass spend 700 ms.
  const Removal shared = runRemoval(kRowCap, 300, kRemoveMs);
  TEST_ASSERT_EQUAL_size_t(5, shared.dropped);
  TEST_ASSERT_EQUAL_UINT32(400, shared.elapsedMs);   // measured from the walk's start
  TEST_ASSERT_EQUAL_size_t(20, early.dropped);       // what a fresh clock would allow
  TEST_ASSERT_LESS_THAN_size_t(early.dropped, shared.dropped);

  // Walk plus removal is one budget and the item in hand, at every split of it.
  for (uint32_t walked = 0; walked <= Rns::kWalkBudgetMs; walked += 25) {
    const Removal r = runRemoval(kRowCap, walked, kRemoveMs);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, r.attempts);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(Rns::kWalkBudgetMs + kRemoveMs, r.elapsedMs);
  }

  // Attempts and not successes. Every key already gone is a `stale` full of
  // remove_path()s that return false without touching flash, so the clock does
  // not move: answered with successes the loop would run all 64 with nothing
  // watching it, which is the count-bounded loop this milestone exists to kill.
  const Removal gone = runRemoval(kRowCap, Rns::kWalkBudgetMs, kRemoveMs,
                                  Rns::kWalkBudgetMs, /*allGone=*/true);
  TEST_ASSERT_EQUAL_size_t(0, gone.dropped);
  TEST_ASSERT_EQUAL_size_t(1, gone.attempts);
  TEST_ASSERT_TRUE(gone.stopped);
}

// ---------------------------------------------------------------------------
// The sweep's share of the budget
// ---------------------------------------------------------------------------

void test_the_rows_give_the_sweep_half_the_budget_when_it_is_ahead() {
  // The share, at one position. Half the budget, and only where taking it all
  // would leave the sweep nothing: the sweep's cursor ahead of the row cursor.
  TEST_ASSERT_EQUAL_UINT32(Rns::kWalkBudgetMs / 2, Rns::rowShareMs(Rns::kWalkBudgetMs));
  TEST_ASSERT_EQUAL_UINT32(200, Rns::rowShareMs(Rns::kWalkBudgetMs));

  // Under the share, at the front of the table with the sweep at 70: still a
  // row.
  TEST_ASSERT_TRUE(Rns::WalkStep::Row ==
                   Rns::walkStep(at(10, true, 0, 70, Rns::rowShareMs(Rns::kWalkBudgetMs) - 1)));
  // Over it, the same position is stepped over for nothing — the sweep does not
  // want position 0 either — which is what lets the walk reach 70 at all.
  const Rns::WalkState spent = at(10, true, 0, 70, Rns::rowShareMs(Rns::kWalkBudgetMs));
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip == Rns::walkStep(spent));
  TEST_ASSERT_TRUE(Rns::rowShareSpent(spent));
  TEST_ASSERT_TRUE(Rns::rowShareDenied(spent));
  // And the position the sweep does want is read for it.
  TEST_ASSERT_TRUE(Rns::WalkStep::Examine ==
                   Rns::walkStep(at(10, true, 70, 70, Rns::rowShareMs(Rns::kWalkBudgetMs))));

  // Not while the two cursors travel together, which is most of a cycle: the
  // same reads serve both halves there, so taking half the budget off the rows
  // would slow both and fix nothing.
  const Rns::WalkState level = at(10, true, 0, 0, Rns::kWalkBudgetMs - 1);
  TEST_ASSERT_FALSE(Rns::rowShareSpent(level));
  TEST_ASSERT_TRUE(Rns::WalkStep::Row == Rns::walkStep(level));
  // Nor with the sweep behind the rows, where the walk reaches its cursor first
  // anyway.
  TEST_ASSERT_FALSE(Rns::rowShareSpent(resumingRowsAt(at(10, true, 80, 40, Rns::kWalkBudgetMs - 1), 60)));
  // Nor on a pass with no sweep running at all, which is eleven passes in
  // twelve: the rows get the whole budget, as they always did.
  TEST_ASSERT_FALSE(Rns::rowShareSpent(at(10, false, 0, 70, Rns::kWalkBudgetMs - 1)));
  // And never once the rows are full — there is nothing left to deny.
  TEST_ASSERT_FALSE(Rns::rowShareDenied(at(kRowCap, true, 0, 70, Rns::kWalkBudgetMs - 1)));

  // End to end, on the table the rest of this suite uses. Seven rows and then
  // the walk hands over: sixty-three positions stepped over for nothing and
  // seven records read at the sweep's cursor, out of the same fourteen reads a
  // pass has always had at this per-record cost.
  const Pass r = runWalk(table(200, true), true, 70, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t(7, r.rows);
  TEST_ASSERT_EQUAL_size_t(14, r.reads);
  TEST_ASSERT_EQUAL_size_t(63, r.skipped);       // 7..69, none of them dereferenced
  TEST_ASSERT_EQUAL_size_t(77, r.stoppedAt);     // the sweep got to 77
  TEST_ASSERT_EQUAL_size_t(7, r.rowStoppedAt);   // the rows got to 7
  TEST_ASSERT_TRUE(r.rowsCut);
  TEST_ASSERT_TRUE(r.ranOut);
  // Without the share this pass read fourteen rows and stopped at 14, and the
  // sweep's cursor at 70 was not reached at all — which is the whole of what
  // follows.
}

void test_the_sweep_cursor_advances_on_every_sweeping_pass() {
  // The guarantee the share exists for, driven over two hundred passes of the
  // table this suite uses — two hundred entries, the first hundred live.
  //
  // Before it, the sweep advanced only in the one pass per row cycle where the
  // row cursor caught up to it: the cursor sat at 70 for five passes, then 76
  // for five, then 82, and the first dead entry was reached on pass 35 with the
  // cycle closing on pass 115. Those are measurements of the pre-share rules,
  // which this tree no longer has — narrative here and in SnapshotWalk.h, and
  // not re-derivable from anything below; only the with-share figures are
  // asserted. With it, the cursor moves on every pass, the
  // first dead entry comes on pass 10 and the cycle closes on pass 23 — and
  // goes round eight times in two hundred passes rather than once.
  std::vector<bool> live = table(200, true);
  for (size_t k = 100; k < live.size(); k++) live[k] = false;

  // The three exact figures below are exact about one arithmetic: at 30 ms a
  // record a pass gets fourteen reads, seven of them rows before the share
  // hands the rest to the sweep. Stated here so that a retune of
  // kWalkBudgetMs, kWalkYieldMs or SNAPSHOT_MAX_PATHS fails on the premise —
  // which says what moved — rather than three lines further down on a bare
  // "expected 10, was 12".
  const Pass shape = runWalk(live, true, 70, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(14, shape.reads,
      "reads per pass at 30 ms a record moved; the pass counts below follow from it");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(7, shape.rows,
      "rows before the share hands over moved; the pass counts below follow from it");

  Node n;
  size_t firstSwept = 0, firstClose = 0, stalls = 0;
  for (size_t passNo = 1; passNo <= 200; passNo++) {
    const size_t before = n.sweepPos;
    const Pass r = onePass(n, live, kFastRecordMs);
    // A sweeping pass either moved the cursor forward or closed the cycle and
    // put it back to the front. Standing still is the failure.
    if (n.sweepPos != 0 && n.sweepPos <= before && passNo > 1) stalls++;
    if (!firstSwept && !r.swept.empty()) firstSwept = passNo;
    if (!firstClose && n.sweepCloses)    firstClose = passNo;
  }
  TEST_ASSERT_EQUAL_size_t(0, stalls);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(10, firstSwept,
      "the first dead entry is reached on a different pass now (was 35 without the share)");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(23, firstClose,
      "the first sweep cycle closes on a different pass now (was 115 without the share)");
  TEST_ASSERT_EQUAL_size_t_MESSAGE(8, n.sweepCloses,
      "cycles closed in 200 passes moved (was 1 without the share)");
  // The rows are not starved for it: they still go all the way round, and every
  // list published is a whole one.
  TEST_ASSERT_GREATER_THAN_size_t(0, n.rowCycles);
  TEST_ASSERT_EQUAL_size_t(kRowCap, n.published);
  TEST_ASSERT_TRUE(n.rowsWhole);
}

void test_the_row_cap_knife_edge_no_longer_stalls_the_sweep() {
  // The pathological case, which is not exotic: where reads per pass divide
  // SNAPSHOT_MAX_PATHS exactly, the row half fills the cap precisely as the
  // budget runs out, so the walk stopped at the cap on every pass and the sweep
  // cursor never moved past it at all. At 25 ms a record a pass reads exactly
  // sixteen, and 64 / 16 = 4.
  TEST_ASSERT_EQUAL_size_t(16, runWalk(table(4000, true), false, 0, 25).reads);
  TEST_ASSERT_EQUAL_size_t(0, kRowCap % 16);

  // Before the share the cursor went 16, 32, 48, 64 and then stayed at 64 for
  // ever: no dead entry beyond it was ever found and no cycle ever closed. It
  // took a change in the per-record cost to break out, which is a cleanup that
  // depends on the filesystem jittering.
  Node n;
  const std::vector<bool> live = table(200, true);
  std::vector<size_t> cursor;
  size_t firstClose = 0;
  for (size_t passNo = 1; passNo <= 40; passNo++) {
    onePass(n, live, 25);
    cursor.push_back(n.sweepPos);
    if (!firstClose && n.sweepCloses) firstClose = passNo;
  }
  TEST_ASSERT_EQUAL_size_t(16, cursor[0]);
  TEST_ASSERT_EQUAL_size_t(32, cursor[1]);
  TEST_ASSERT_EQUAL_size_t(48, cursor[2]);
  TEST_ASSERT_EQUAL_size_t(64, cursor[3]);
  // The knife edge itself: the pass that used to stop at the cap and stay
  // there. It goes past it by the records the sweep's half of the budget buys
  // — 200 ms at 25 ms each is eight positions — which is what the share is.
  TEST_ASSERT_EQUAL_size_t_MESSAGE(kRowCap + Rns::rowShareMs(Rns::kWalkBudgetMs) / 25,
      cursor[4], "the pass that broke the knife edge moved; was 64, and 64 for ever after");
  TEST_ASSERT_EQUAL_size_t(80, cursor[5]);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(21, firstClose,
      "the pass the cycle first closes on moved; it never closed at all before the share");
  TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, n.sweepCloses);
}

void test_a_cycle_the_share_cut_short_is_kept_as_a_prefix() {
  // The hazard the share creates, and the reason the row half carries its own
  // "was it cut short" rather than borrowing the walk's. A pass that hands over
  // to the sweep and then runs off the end of the table has *not* run out of
  // budget — and a list judged on that alone would be published as a whole
  // cycle. Here that is seven rows on a table of seventy-five.
  const Pass r = runWalk(table(75, true), true, 70, kFastRecordMs);
  TEST_ASSERT_FALSE(r.ranOut);                  // it reached the end of the table
  TEST_ASSERT_TRUE(r.rowsCut);                  // but the rows did not
  TEST_ASSERT_EQUAL_size_t(7, r.rows);
  TEST_ASSERT_EQUAL_size_t(7, r.rowStoppedAt);
  // Judged on the walk alone, this publishes a seven-row list as complete.
  TEST_ASSERT_TRUE(Rns::rowCycleDone(r.rows, kRowCap, r.ranOut));
  // Judged on the row half's own answer, it is the prefix it is.
  TEST_ASSERT_FALSE(Rns::rowCycleDone(r.rows, kRowCap, r.ranOut || r.rowsCut));
  // And the row cursor follows the rows rather than the walk: 7, not 75, which
  // would have skipped everything between them for a whole cycle.
  TEST_ASSERT_EQUAL_size_t(7, Rns::nextCursor(0, r.rowStoppedAt, r.ranOut || r.rowsCut));
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextCursor(0, r.stoppedAt, r.ranOut));

  // The sweep, on the same pass, did reach the end and its cycle did close.
  // The two halves answer separately, which is the point.
  TEST_ASSERT_EQUAL_size_t(75, r.stoppedAt);
}

void test_a_record_over_the_row_share_can_still_deny_the_sweep() {
  // Where the share's guarantee stops, which is not where the header used to
  // say it did. "A single record that costs the whole budget" is the uniform
  // reading; the boundary for mixed costs is half of it, and mixed is the real
  // case — one store was measured at 93-162 ms a record, not at a constant
  // (FileStore.h).
  // 200 ms, pinned against the budget by
  // test_the_rows_give_the_sweep_half_the_budget_when_it_is_ahead above.
  const uint32_t share = Rns::rowShareMs(Rns::kWalkBudgetMs);

  // Uniform, and well over the share: the sweep is served anyway. The first
  // record spends the whole share by itself, so the walk hands over at 250 ms
  // and reaches the cursor at 70 with a third of the budget still in hand.
  const Pass uniform = runWalk(table(200, true), true, 70, 250);
  TEST_ASSERT_EQUAL_size_t(2, uniform.reads);       // one row, then the sweep's own
  TEST_ASSERT_EQUAL_size_t(71, uniform.stoppedAt);
  TEST_ASSERT_EQUAL_size_t(71, Rns::nextCursor(70, uniform.stoppedAt, uniform.ranOut));

  // The same 250 ms among cheap ones, which is the case that fails: sixty-three
  // 3 ms records creep the clock to just under the share, the sixty-fourth
  // costs 250, and the pass is past the budget before it reaches position 70.
  // The cursor stands still for this pass — the one thing the share exists to
  // prevent, and it takes half the cost the uniform reading asks for.
  std::vector<uint32_t> costs(kRowCap, 3);
  costs[kRowCap - 1] = 250;
  const Pass mixed = runWalk(table(200, true), true, 70, 3, Rns::kWalkYieldMs, 1,
                             Rns::kWalkBudgetMs, 0, 0, 0, &costs);
  TEST_ASSERT_TRUE_MESSAGE(mixed.elapsedMs - 250 < share,
      "the cheap records no longer land the pass just under the share; retune them");
  TEST_ASSERT_TRUE(mixed.ranOut);
  TEST_ASSERT_EQUAL_size_t(kRowCap, mixed.stoppedAt);   // short of the cursor at 70
  TEST_ASSERT_EQUAL_size_t(70, Rns::nextCursor(70, mixed.stoppedAt, mixed.ranOut));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkBudgetMs, mixed.elapsedMs);

  // The turning point as the rule rather than as a scenario. The last position
  // the rows are served at is the last one under the share, so from there a
  // record of exactly the share leaves the pass one millisecond inside the
  // budget and one millisecond more does not.
  TEST_ASSERT_TRUE(Rns::WalkStep::Row == Rns::walkStep(at(1, true, 5, 70, share - 1)));
  TEST_ASSERT_TRUE(Rns::WalkStep::Skip ==
                   Rns::walkStep(at(1, true, 6, 70, share - 1 + share)));
  TEST_ASSERT_TRUE(Rns::WalkStep::StopBudget ==
                   Rns::walkStep(at(1, true, 6, 70, share - 1 + share + 1)));

  // So the margin this tree actually has is against 200 ms and not 400: the
  // worst per-record figure measured anywhere in it is 81 % of the share.
  TEST_ASSERT_LESS_THAN_UINT32(share, kSlowRecordMs);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(81, kSlowRecordMs * 100 / share,
      "the worst measured record is no longer 81 % of the row share; the margin "
      "paragraph in SnapshotWalk.h quotes that figure");
}

// ---------------------------------------------------------------------------
// Scheduling the next pass against what this one cost
// ---------------------------------------------------------------------------

void test_the_scheduler_is_inert_at_every_cost_this_tree_has_measured() {
  // The measured half of the same quarter walkBudgetFitsInterval() applies to
  // the budget. On any node whose pass costs less than that quarter it hands
  // the configured interval straight back, and it must: a scheduler that moved
  // the interval on a healthy node would be a tuning knob rather than a guard.
  TEST_ASSERT_EQUAL_UINT32(4, Rns::kPassShareDiv);
  const uint32_t interval = (uint32_t)SNAPSHOT_INTERVAL_MS;
  const uint32_t ceiling  = kSweepCeilingMs;     // kStaleSweepMs, the caller's own

  // The budget plus the worst record anywhere in this tree, which is what a
  // pass costs when the budget ends it.
  const uint32_t worstMeasured = Rns::kWalkBudgetMs + kSlowRecordMs;
  TEST_ASSERT_EQUAL_UINT32(562, worstMeasured);
  TEST_ASSERT_EQUAL_UINT32(interval, Rns::nextIntervalMs(worstMeasured, interval, ceiling));
  TEST_ASSERT_EQUAL_UINT32(interval, Rns::nextIntervalMs(0, interval, ceiling));
  // Right up to the quarter, and one millisecond past it is where it starts.
  TEST_ASSERT_EQUAL_UINT32(interval, Rns::nextIntervalMs(interval / 4, interval, ceiling));
  TEST_ASSERT_EQUAL_UINT32(5004, Rns::nextIntervalMs(interval / 4 + 1, interval, ceiling));
}

void test_a_pass_that_outgrows_its_share_buys_itself_room() {
  const uint32_t interval = (uint32_t)SNAPSHOT_INTERVAL_MS;
  const uint32_t ceiling  = kSweepCeilingMs;

  // The case this exists for, and it is not hypothetical: passMayStop() may not
  // end a pass that has read nothing, so a pass costs the prefix it steps over
  // plus one record if stepping ever stops being free — which is the microStore
  // the abandoned branch ran on. This is the very pass
  // test_a_pass_reaches_its_own_cursor_even_if_stepping_stops_being_free
  // drives: fifty positions at 30 ms and one record.
  const Pass overrun = runWalk(table(200, true), false, 0, kFastRecordMs, Rns::kWalkYieldMs,
                               1, Rns::kWalkBudgetMs, /*rowPos=*/50, /*rowsAlready=*/50,
                               /*skipMs=*/kFastRecordMs);
  TEST_ASSERT_GREATER_THAN_UINT32(Rns::kWalkBudgetMs, overrun.elapsedMs);
  TEST_ASSERT_GREATER_THAN_UINT32(interval / 4, overrun.elapsedMs);
  TEST_ASSERT_EQUAL_UINT32(overrun.elapsedMs * 4,
                           Rns::nextIntervalMs(overrun.elapsedMs, interval, ceiling));

  // And the figure that took a T-Beam down: 7.5 s a walk against a 5 s gate is
  // a gate that is already open when the pass returns, so passes ran back to
  // back. Four times the cost is 30 s, and the ceiling is not reached.
  TEST_ASSERT_EQUAL_UINT32(30000, Rns::nextIntervalMs(7500, interval, ceiling));
}

void test_the_scheduler_has_a_ceiling_the_abandoned_formula_did_not() {
  const uint32_t interval = (uint32_t)SNAPSHOT_INTERVAL_MS;
  const uint32_t ceiling  = kSweepCeilingMs;

  // max(walkMs * 4, SNAPSHOT_INTERVAL_MS) — the version on the abandoned branch
  // — has no upper end, so a node whose passes cost a minute would be put on a
  // four-minute interval, and a node that slow needs its cleanup more often
  // than that rather than less.
  TEST_ASSERT_GREATER_THAN_UINT32(ceiling, ceiling * Rns::kPassShareDiv);
  TEST_ASSERT_EQUAL_UINT32(ceiling, Rns::nextIntervalMs(ceiling, interval, ceiling));
  // The clamp begins at a quarter of the ceiling, where four times the cost
  // would first exceed it.
  TEST_ASSERT_EQUAL_UINT32(ceiling, Rns::nextIntervalMs(ceiling / 4, interval, ceiling));
  TEST_ASSERT_EQUAL_UINT32(59996, Rns::nextIntervalMs(ceiling / 4 - 1, interval, ceiling));
  // It never returns less than the configured interval, whatever it is asked.
  for (uint32_t cost = 0; cost <= 20000; cost += 137)
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(interval,
                                        Rns::nextIntervalMs(cost, interval, ceiling));
  // A ceiling below the interval is a misconfiguration and is answered with the
  // interval rather than with something shorter than the node was told to use.
  TEST_ASSERT_EQUAL_UINT32(interval, Rns::nextIntervalMs(9000, interval, 1000));
  // No multiply is reached above the ceiling's own quarter, which is what keeps
  // the arithmetic from wrapping at absurd inputs — the same reason
  // walkBudgetFitsInterval() divides rather than multiplies.
  TEST_ASSERT_EQUAL_UINT32(ceiling, Rns::nextIntervalMs(0xFFFFFFFFu, interval, ceiling));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu,
                           Rns::nextIntervalMs(0xFFFFFFFFu, interval, 0xFFFFFFFFu));
}

void test_passes_cannot_run_back_to_back_under_the_worst_timing() {
  // The property the static 12.5x margin cannot give, because that margin is
  // between the *budget* and the interval and a pass is not its budget. The
  // gate is "has the interval passed since the *start* of the last pass", so
  // what the RNS task gets back is interval - cost: the time it spends going
  // round its loop and sleeping 10 ms at a time, which is the only core the
  // priority-1 loopTask gets (SnapshotWalk.h).
  const uint32_t interval = (uint32_t)SNAPSHOT_INTERVAL_MS;
  const uint32_t ceiling  = kSweepCeilingMs;

  // A pass that outlasts the interval leaves nothing at all under the fixed
  // gate — the gate is open the moment the pass returns and the next one starts
  // at once. That is the shape that took the T-Beam down.
  TEST_ASSERT_TRUE(7500u > interval);
  // Scheduled against what it cost, the same pass leaves 22.5 s.
  TEST_ASSERT_EQUAL_UINT32(22500, Rns::nextIntervalMs(7500, interval, ceiling) - 7500);

  // The general form: up to the ceiling's quarter, the gap is at least four
  // times the pass, so the task is out of this function for at least three
  // quarters of every window whatever a pass comes to cost.
  // Every cost in the linear branch rather than a sample of them. The property
  // is an algebraic identity inside that branch, so a stride proves nothing a
  // step does not — and the branch is fifteen thousand values, which costs
  // this suite nothing.
  for (uint32_t cost = 0; cost <= ceiling / Rns::kPassShareDiv; cost++) {
    const uint32_t next = Rns::nextIntervalMs(cost, interval, ceiling);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(cost * Rns::kPassShareDiv, next);
    // ...and so out of it for all but one share of every window.
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(cost * (Rns::kPassShareDiv - 1), next - cost);
  }
  // Past that quarter the ceiling takes over and the margin is given up on
  // purpose: a node spending a quarter of a minute in one pass is broken in a
  // way scheduling cannot fix, and holding the ratchet there would put its
  // dead-path cleanup further out than the minute it is supposed to run on.
  // What is kept is that the interval never shrinks and never runs away.
  TEST_ASSERT_LESS_THAN_UINT32(45000u * 4, Rns::nextIntervalMs(45000, interval, ceiling));
  TEST_ASSERT_EQUAL_UINT32(ceiling, Rns::nextIntervalMs(45000, interval, ceiling));
}

// ---------------------------------------------------------------------------
// The yield cadence
// ---------------------------------------------------------------------------

void test_the_yield_cadence_fires_more_than_once_inside_one_budget() {
  // At the cost this firmware links, one budget's walk yields three times.
  // Where those three land is asserted below, and is not the tidy 100/200/300.
  const Pass fast = runWalk(table(200, true), false, 0, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t(3, fast.yields);
  // Where they land, which the header quotes and used to divide out wrongly.
  // Not 100, 200 and 300: the cadence is measured from the previous yield, and
  // a yield costs a tick that is itself charged to the budget, so the gap
  // between two of them is a record's worth over the interval rather than
  // exactly it. Driven here so the header cannot drift back to the tidy
  // version.
  TEST_ASSERT_EQUAL_size_t(3, fast.yieldAt.size());
  TEST_ASSERT_EQUAL_UINT32(120, fast.yieldAt[0]);
  TEST_ASSERT_EQUAL_UINT32(241, fast.yieldAt[1]);
  TEST_ASSERT_EQUAL_UINT32(362, fast.yieldAt[2]);
  // The rule they come from, at each landing: at or past a cadence since the
  // one before, and never before it.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkYieldMs, fast.yieldAt[0]);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkYieldMs, fast.yieldAt[1] - fast.yieldAt[0]);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(Rns::kWalkYieldMs, fast.yieldAt[2] - fast.yieldAt[1]);
  // And at the worst per-record cost the tree records, where a pass reads only
  // a handful of records at all. Still more than once, which is the property.
  const Pass slow = runWalk(table(200, true), false, 0, kSlowRecordMs);
  TEST_ASSERT_EQUAL_size_t(2, slow.yields);
  TEST_ASSERT_GREATER_THAN_size_t(1, fast.yields);
  TEST_ASSERT_GREATER_THAN_size_t(1, slow.yields);
}

void test_the_feed_cadence_would_not_yield_at_all() {
  // Why the yield is not Sys::RingDrain::kFeedEvery. That cadence is a count of
  // records; 16 of them at 30 ms is 480 ms, which is longer than the whole
  // budget, so the walk is stopped before it ever reaches one. It is also why
  // the walk no longer *feeds* on that cadence: the same sixteenth record is
  // the one a feed on it would have waited for.
  TEST_ASSERT_EQUAL_size_t(16, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_GREATER_THAN_UINT32(Rns::kWalkBudgetMs,
                                  (uint32_t)Sys::RingDrain::kFeedEvery * kFastRecordMs);

  // Driven rather than argued: the same walk with a count-of-16 cadence.
  size_t reads = 0, yields = 0;
  uint32_t elapsedMs = 0;
  for (size_t pos = 0; pos < 200; pos++) {
    if (Rns::walkStep(at(reads, false, pos, 0, elapsedMs)) != Rns::WalkStep::Row) break;
    if (++reads % Sys::RingDrain::kFeedEvery == 0) yields++;
    elapsedMs += kFastRecordMs;
  }
  TEST_ASSERT_EQUAL_size_t(0, yields);
  TEST_ASSERT_LESS_THAN_size_t(Sys::RingDrain::kFeedEvery, reads);

  // The elapsed-time cadence on the identical walk gives the core up three
  // times. That difference is the fix.
  const Pass r = runWalk(table(200, true), false, 0, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t(3, r.yields);
}

void test_the_yield_ceiling_does_not_move_with_the_record_cost() {
  // The property no count of records has: however cheap a record becomes, a
  // pass gives the core up at most three times, because a fourth would need the
  // whole budget elapsed and that is a stop.
  for (uint32_t recordMs = 0; recordMs <= 40; recordMs++) {
    const Pass r = runWalk(table(4000, true), false, 0, recordMs);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(3, r.yields);
  }
  // A free record is the extreme of that, and it needs a sweep running to be
  // the case it claims: with no sweep the walk stops at the row cap after 64
  // positions and the 4000 are never reached. Sweeping, it reads all 4000 —
  // and because nothing moves the clock it never reaches a yield at all, which
  // is the ceiling holding from below rather than above.
  const Pass free = runWalk(table(4000, true), true, 0, 0);
  TEST_ASSERT_FALSE(free.ranOut);
  TEST_ASSERT_EQUAL_size_t(4000, free.reads);
  TEST_ASSERT_EQUAL_size_t(0, free.yields);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(3, free.yields);
}

void test_the_first_record_of_a_pass_does_not_yield() {
  // lastYieldMs starts at the walk's own start, so a short walk pays nothing.
  TEST_ASSERT_FALSE(Rns::walkShouldYield(0, 0));
  TEST_ASSERT_FALSE(Rns::walkShouldYield(Rns::kWalkYieldMs - 1, 0));
  TEST_ASSERT_TRUE(Rns::walkShouldYield(Rns::kWalkYieldMs, 0));
  // And the interval is measured from the last yield, not from the start.
  TEST_ASSERT_FALSE(Rns::walkShouldYield(Rns::kWalkYieldMs + 1, Rns::kWalkYieldMs));
  TEST_ASSERT_TRUE(Rns::walkShouldYield(Rns::kWalkYieldMs * 2, Rns::kWalkYieldMs));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_constants_are_what_the_fix_committed_to);
  RUN_TEST(test_the_budget_stays_well_inside_the_interval_a_pass_is_gated_on);
  RUN_TEST(test_a_budget_that_outlasts_the_interval_is_what_the_guard_is_for);
  RUN_TEST(test_the_budget_stops_a_pass_while_rows_are_still_filling);
  RUN_TEST(test_the_budget_applies_on_the_skip_path);
  RUN_TEST(test_rows_full_and_no_sweep_is_a_finish_not_a_budget_stop);
  RUN_TEST(test_a_position_neither_half_wants_is_stepped_over);
  RUN_TEST(test_a_row_is_read_whether_or_not_the_sweep_wants_it);
  RUN_TEST(test_a_slow_table_is_bounded_by_the_budget_in_the_row_half);
  RUN_TEST(test_a_table_of_dead_entries_is_bounded_too);
  RUN_TEST(test_a_small_table_finishes_inside_the_budget_and_closes_the_cycle);
  RUN_TEST(test_a_pass_stops_on_rows_full_when_nothing_else_needs_the_rest);
  RUN_TEST(test_a_sweeping_pass_steps_over_the_prefix_it_has_already_examined);
  RUN_TEST(test_the_sweep_cursor_never_moves_backwards);
  RUN_TEST(test_a_row_bound_pass_leaves_the_cursor_where_it_was);
  RUN_TEST(test_a_cursor_past_the_end_of_the_table_starts_the_cycle_again);
  RUN_TEST(test_a_cursor_left_past_a_shrunken_table_does_not_latch);
  RUN_TEST(test_a_row_bound_table_now_advances_the_cursor);
  RUN_TEST(test_a_pass_may_not_stop_on_budget_before_it_has_read_something);
  RUN_TEST(test_a_pass_reaches_its_own_cursor_even_if_stepping_stops_being_free);
  RUN_TEST(test_a_resuming_pass_does_not_end_at_its_own_front_door);
  RUN_TEST(test_only_a_whole_list_is_published);
  RUN_TEST(test_a_pass_that_threw_starts_the_row_cycle_again);
  RUN_TEST(test_the_removal_is_bounded_by_the_walks_clock_and_not_a_fresh_one);
  RUN_TEST(test_the_rows_give_the_sweep_half_the_budget_when_it_is_ahead);
  RUN_TEST(test_the_sweep_cursor_advances_on_every_sweeping_pass);
  RUN_TEST(test_the_row_cap_knife_edge_no_longer_stalls_the_sweep);
  RUN_TEST(test_a_cycle_the_share_cut_short_is_kept_as_a_prefix);
  RUN_TEST(test_a_record_over_the_row_share_can_still_deny_the_sweep);
  RUN_TEST(test_the_scheduler_is_inert_at_every_cost_this_tree_has_measured);
  RUN_TEST(test_a_pass_that_outgrows_its_share_buys_itself_room);
  RUN_TEST(test_the_scheduler_has_a_ceiling_the_abandoned_formula_did_not);
  RUN_TEST(test_passes_cannot_run_back_to_back_under_the_worst_timing);
  RUN_TEST(test_the_yield_cadence_fires_more_than_once_inside_one_budget);
  RUN_TEST(test_the_feed_cadence_would_not_yield_at_all);
  RUN_TEST(test_the_yield_ceiling_does_not_move_with_the_record_cost);
  RUN_TEST(test_the_first_record_of_a_pass_does_not_yield);
  return UNITY_END();
}
