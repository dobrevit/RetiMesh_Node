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
// 400 ms is picked to be obviously survivable rather than optimal: 75x inside
// the 30 s watchdog (WATCHDOG_TIMEOUT_S), and well under a tenth of the five
// seconds between passes (SNAPSHOT_INTERVAL_MS). It is checked between records
// and never inside one, so a pass costs it plus however long the record in
// hand takes.
//
// It now bounds the whole walk rather than only the part after the rows are
// full. What that costs, until row collection can be resumed across passes, is
// worth stating in full rather than as "the reading is late". Rows are
// collected from the front of the table on every pass, so on a table slow
// enough to spend the budget before the rows fill, every pass reads the same
// short prefix and stops in the same place: the published list is not late, it
// is permanently a prefix; the sweep cursor never gets past that prefix; the
// cycle never closes; and a dead entry beyond it is never found at all. The
// regime is exactly rowCap x per-record cost > kWalkBudgetMs — with 64 rows,
// any store charging more than 6.25 ms a record, and lower still on a table
// with dead entries in it, which cost a read and fill no row. At the 30 ms a
// record measured on a 200-record store (platformio.ini), a pass reads
// fourteen. What that fourteen is not is a crossover: both per-record figures
// in this file were taken on a 200-record store, and nothing in this tree
// measures what a record costs on a table of ten, so the count a smaller table
// reads does not follow from either. The regime a sentence above is the part
// that is unarguable; where the real crossover falls is a measurement nobody
// has taken. test_a_row_bound_table_never_advances_the_cursor drives the
// regime, and is written to be flipped by the pass that resumes row collection
// across passes.
//
// What is bought for that, and why the bound is taken anyway: pathCount() is
// the whole table and is unaffected, no pass is long, and the node stays up.
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
// driven rather than argued in test/test_snapshot_walk. The same arithmetic is
// why the walk no longer *feeds* on that cadence either (RingDrain.h): a
// bounded pass needs no feed inside it, and this one could not have had one.
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
// A yield is vTaskDelay(1), deliberately a bare tick count and not
// pdMS_TO_TICKS(1). CONFIG_FREERTOS_HZ is 1000 in the esp32 and esp32s3
// sdkconfigs of framework-arduinoespressif32-libs, so today the two would
// compile to the same thing; at any lower tick rate pdMS_TO_TICKS(1) truncates
// to zero, and vTaskDelay(0) does not block at all — it yields to tasks of this
// task's priority and above, which is every task except the one the yield
// exists for. A bare 1 is one tick whatever a tick is worth.
//
// What one yield is worth, exactly: vTaskDelay(1) blocks until the *next tick
// interrupt*, so it hands the core down for anywhere between nearly nothing and
// 1 ms depending where in the tick period the call lands, and the priority-3
// RNS task preempts loopTask again the moment that tick readies it. Three of
// them are therefore 0-3 ms of core 1 per pass, with a floor of nearly nothing.
//
// And a yield hands the core to the highest-priority *ready* task, which need
// not be the one it was meant for: the radio task is priority 5 on core 1
// (main.cpp), above this one and far above loopTask, so a yield that lands
// while a frame is being drained is worth loopTask nothing at all. Which is
// another way of saying the same thing — the yields are a margin, not a
// guarantee, and nothing below should be read as one.
//
// So the yields are a margin inside the walk. They are not what stops loopTask
// starving — the budget is, and by a different mechanism entirely.
// refreshSnapshots() returns immediately unless SNAPSHOT_INTERVAL_MS (5000 ms)
// has passed since the *start* of the last pass, and the RNS task's own loop
// ends in vTaskDelay(pdMS_TO_TICKS(10)) (main.cpp). With the walk bounded, a
// pass is a fraction of that window and the task spends the rest of it going
// round its loop and sleeping those 10 ms, hundreds of times per window — and
// every one of those sleeps is core 1 for the tasks below it, loopTask among
// them. The walk that took the node down did not leave any: 7.5 s on the T-Beam
// is longer than the interval, so the gate was already open when the pass
// returned and the next walk began at once. Passes ran back to back, and
// loopTask got the one 10 ms sleep per 7.5 s of walking that separated them,
// against a watchdog of 30 s (WATCHDOG_TIMEOUT_S).
//
// Which on its own does not yet predict a reboot: a task that gets 10 ms every
// 7.5 s and fed at the top of its body would feed every 7.5 s, comfortably
// inside 30 s. The step that closes it is that a preempted task resumes
// *mid-body*, not at the top — Watchdog::feed() is the first statement of
// loop() (main.cpp), so loopTask has to accumulate a whole loop() body of CPU
// out of those 10 ms slices before it reaches the feed again, and each slice is
// 7.5 s from the next. That body is not small: Rns::Inbox::poll() writes a
// queued message to flash, Imu, Compass and Environment each poll a part on
// I2C, and ConsoleServer::poll() and Maintenance::poll() run before them. A
// body wanting more CPU than three or four slices carry is therefore 30 s or
// more between one feed and the next, and that is the reboot.
//
// Milliseconds per pass is what the yields add to that; seconds per
// five-second window is what the budget returns, and the second number is the
// rescue.
//
// Which is why kWalkBudgetMs must stay well inside SNAPSHOT_INTERVAL_MS. The
// hazard itself is budget + worst record reaching the interval: at that point
// the gate is open again the moment the pass returns, the back-to-back passes
// are back, and no yield cadence rescues that. Someone buying rows back with a
// 6 s budget would get 49 yields to a pass rather than three, and they would
// still be worth at most a millisecond each — under 50 ms of core 1 for every
// 6 s of walking, against a walk that once again never stops. (Forty-nine and
// not sixty: a yield costs a tick that is itself charged to the budget, so at
// 30 ms a record the yields land about 121 ms apart rather than 100. The figure
// is driven in test_snapshot_walk rather than divided out here, which is how it
// was found to be wrong.)
//
// The quarter walkBudgetFitsInterval() enforces is not that cliff edge; it is a
// deliberate 4x margin short of it, so the guard fires on a retune that is
// merely unwise rather than only on one that is already fatal. A 3 s budget
// fails it and should: 3 s of walking to 2 s of loop is a node spending most of
// its time in this function. But 3 s does not reproduce the failure — the pass
// ends 2 s before the gate reopens, and the RNS task still reaches the 10 ms
// sleep at the end of its loop about two hundred times in that remainder.
//
// So the relation is asserted rather than asked for (walkBudgetFitsInterval()
// below), and it is
// worth asserting because nothing else would catch a repeat: core 1's idle
// task is not watched (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is unset in
// both sdkconfigs, while CPU0's is set), so loopTask's own subscription in
// setup() is the only thing that would notice.
constexpr uint32_t kWalkYieldMs = 100;

// The relation the choice above rests on. A cadence that does not fit strictly
// more than twice in a budget gives the core up at most once a pass: the second
// yield needs 2 x the interval to have elapsed, and elapsing the whole budget
// is a stop rather than a yield.
static_assert(kWalkYieldMs > 0 && kWalkYieldMs * 2 < kWalkBudgetMs,
              "the yield cadence must fire more than once inside one budget");

// The relation the paragraph above rests on, for the caller to assert: a walk
// has to end well inside the interval its pass is gated on, or passes run back
// to back and the RNS task never reaches the sleep that is what lets loopTask
// run at all. A quarter is where the line is drawn — at 400 ms against 5000 ms
// the margin is 12.5x, and even at the limit a walk ends a quarter of the way
// into a window, plus the record in hand.
//
// A function rather than a static_assert here because SNAPSHOT_INTERVAL_MS is
// Config.h's and this header depends on nothing (see the top of the file).
// RnsTransport.cpp static_asserts it against the real figure beside the call,
// and test_snapshot_walk asserts the same thing on the host.
//
// Divided rather than multiplied. The two are the same relation for every pair
// of integers that does not overflow, and only one of them stays that way: an
// absurd budget makes kWalkBudgetMs * 4 wrap and the guard answer "fits".
constexpr bool walkBudgetFitsInterval(uint32_t intervalMs) {
  return kWalkBudgetMs <= intervalMs / 4;
}

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

// Where a sweep resumes at the top of a pass, given how big the table is now.
//
// The cursor is an offset into a table that is mutated between passes, so it
// can be left pointing past the end of one: the caller drops up to
// SNAPSHOT_MAX_PATHS entries at the end of a pass, which can take the table out
// from under a cursor that was beyond them. Such a cursor is reached by no
// position, and nothing downstream corrects it — nextSweepPos() below only ever
// moves forwards, so on a table slow enough for the budget to end every pass it
// would keep returning that same stale value: the sweep would stay part-way
// through for ever, the cycle would never close, and the minute clock that
// starts the next one would never be stamped again.
//
// So a cursor at or past the end of the table is not a cursor, and the cycle
// starts again from the front. Asked before the caller decides whether it is
// sweeping, so the pass that finds it stale is the one that starts afresh.
inline size_t sweepResumePos(size_t sweepPos, size_t tableSize) {
  return sweepPos < tableSize ? sweepPos : 0;
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
//
// Forwards only is a ratchet, and a ratchet has to be answerable to the thing
// it indexes: sweepResumePos() above is what keeps it from ratcheting into a
// table that has since shrunk past it.
inline size_t nextSweepPos(size_t sweepPos, size_t stoppedAt, bool ranOut) {
  if (!ranOut) return 0;
  return stoppedAt > sweepPos ? stoppedAt : sweepPos;
}

} // namespace Rns
