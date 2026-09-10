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
//     and not a budget stop, because only a budget stop writes the sweep
//     cursor;
//   * the sweep cursor never moves backwards, which is a hazard the first of
//     those creates: a stop in the row half is at the front of the table, and
//     writing it in would drag the sweep there and leave the tail unexamined;
//   * and the yield cadence really does give the core up several times inside
//     one budget at the per-record costs this repository has measured — the
//     exact thing reusing Sys::RingDrain::kFeedEvery would have failed at.
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
#include "Config.h"                       // SNAPSHOT_MAX_PATHS
#include "../../src/sys/RingDrain.h"      // kFeedEvery, the cadence that will not do
#include "../../src/rns/SnapshotWalk.h"

namespace {

// The firmware's row cap, not a number of this suite's own: a retune of
// SNAPSHOT_MAX_PATHS must move what is asserted here with it.
constexpr size_t kRowCap = SNAPSHOT_MAX_PATHS;

// Per-record costs, sourced above.
constexpr uint32_t kFastRecordMs = 30;
constexpr uint32_t kSlowRecordMs = 162;

// ---------------------------------------------------------------------------
// One position's inputs, spelled out so a test reads as a sentence.
// ---------------------------------------------------------------------------
Rns::WalkState at(size_t rows, bool sweeping, size_t pos, size_t sweepPos,
                  uint32_t elapsedMs) {
  Rns::WalkState w;
  w.rowsCollected = rows;
  w.rowCap        = kRowCap;
  w.sweeping      = sweeping;
  w.pos           = pos;
  w.sweepPos      = sweepPos;
  w.elapsedMs     = elapsedMs;
  w.budgetMs      = Rns::kWalkBudgetMs;
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
// microStore itself).
// ---------------------------------------------------------------------------
struct Pass {
  size_t   rows      = 0;      // rows the pass collected
  size_t   reads     = 0;      // records dereferenced
  size_t   skipped   = 0;      // positions stepped over for free
  size_t   yields    = 0;
  size_t   stoppedAt = 0;      // the position the walk left off at
  bool     ranOut    = false;  // the budget ended it
  bool     rowsFull  = false;  // it ended because there was nothing left to do
  uint32_t elapsedMs = 0;
  std::vector<size_t> swept;   // dead entries the pass found while sweeping
};

Pass runWalk(const std::vector<bool>& live, bool sweeping, size_t sweepPos,
             uint32_t recordMs, uint32_t yieldMs = Rns::kWalkYieldMs,
             uint32_t tickMs = 1) {
  Pass r;
  uint32_t lastYieldMs = 0;
  for (size_t pos = 0; pos < live.size(); pos++) {
    const Rns::WalkState w =
        at(r.rows, sweeping, pos, sweepPos, r.elapsedMs);
    const Rns::WalkStep step = Rns::walkStep(w);
    r.stoppedAt = pos;
    if (step == Rns::WalkStep::StopRowsFull) { r.rowsFull = true; return r; }
    if (step == Rns::WalkStep::StopBudget)   { r.ranOut   = true; return r; }
    if (step == Rns::WalkStep::Skip)         { r.skipped++; continue; }

    if (Rns::walkShouldYield(w.elapsedMs, lastYieldMs, yieldMs)) {
      lastYieldMs = w.elapsedMs;
      r.yields++;
      r.elapsedMs += tickMs;               // a yield is paid out of the budget
    }
    r.reads++;
    r.elapsedMs += recordMs;
    if (!live[pos]) { if (sweeping) r.swept.push_back(pos); continue; }
    if (step == Rns::WalkStep::Row) r.rows++;
  }
  r.stoppedAt = live.size();               // the walk reached the end of the table
  return r;
}

std::vector<bool> table(size_t n, bool allLive) { return std::vector<bool>(n, allLive); }

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
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextSweepPos(0, r.stoppedAt, r.ranOut));
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
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextSweepPos(60, 13, true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextSweepPos(60, 0,  true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextSweepPos(60, 59, true));
  // Forwards it does move.
  TEST_ASSERT_EQUAL_size_t(61, Rns::nextSweepPos(60, 61, true));
  TEST_ASSERT_EQUAL_size_t(60, Rns::nextSweepPos(60, 60, true));
  // And reaching the end of the table closes the cycle whatever the position.
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextSweepPos(60, 999, false));
  TEST_ASSERT_EQUAL_size_t(0, Rns::nextSweepPos(60, 0,   false));
}

void test_a_row_bound_pass_leaves_the_cursor_where_it_was() {
  // End to end: a table slow enough that the budget runs out in the row half,
  // with a sweep already part-way down it. Written out as the firmware writes
  // it — the cursor is fed the position the walk stopped at.
  const size_t cursor = 60;
  const Pass r = runWalk(table(200, true), true, cursor, kFastRecordMs);
  TEST_ASSERT_TRUE(r.ranOut);
  TEST_ASSERT_LESS_THAN_size_t(cursor, r.stoppedAt);        // stopped ahead of the cursor
  TEST_ASSERT_EQUAL_size_t(cursor, Rns::nextSweepPos(cursor, r.stoppedAt, r.ranOut));
  // Without the clamp this pass would have dragged the sweep back to r.stoppedAt
  // and the tail of the table would never be examined again.
  TEST_ASSERT_LESS_THAN_size_t(cursor, r.stoppedAt);
}

// ---------------------------------------------------------------------------
// The yield cadence
// ---------------------------------------------------------------------------

void test_the_yield_cadence_fires_more_than_once_inside_one_budget() {
  // At the cost this firmware links: yields at the first record past 100, 200
  // and 300 ms of walk time.
  const Pass fast = runWalk(table(200, true), false, 0, kFastRecordMs);
  TEST_ASSERT_EQUAL_size_t(3, fast.yields);
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
  // budget, so the walk is stopped before it ever reaches one.
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
  // A free record is the extreme of that: 4000 positions, no reading cost, and
  // still only the yields' own ticks are spent.
  const Pass free = runWalk(table(4000, true), false, 0, 0);
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
  RUN_TEST(test_the_yield_cadence_fires_more_than_once_inside_one_budget);
  RUN_TEST(test_the_feed_cadence_would_not_yield_at_all);
  RUN_TEST(test_the_yield_ceiling_does_not_move_with_the_record_cost);
  RUN_TEST(test_the_first_record_of_a_pass_does_not_yield);
  return UNITY_END();
}
