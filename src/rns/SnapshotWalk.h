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
// It bounds the whole walk rather than only the part after the rows are full,
// and it bounds the removal that follows the walk as well — one budget on one
// clock over the whole pass (see passMayStop() below). The removal used to sit
// outside it on the grounds that what it removes is capped by count at
// SNAPSHOT_MAX_PATHS, which is the same "bounded by count is not bounded by
// time" the paragraph at the top of this file is about: each removal is a
// tombstone written and flushed to flash and an index entry written and flushed
// after it, so 64 of them is 128 write-and-sync pairs with nothing watching the
// clock.
//
// Bounding the rows cost the walk its ability to finish. Rows used to be
// collected from the front of the table on every pass, so on a table slow
// enough to spend the budget before the rows fill, every pass read the same
// short prefix and stopped in the same place: the published list was not late,
// it was permanently a prefix; the sweep cursor never got past that prefix; the
// cycle never closed; and a dead entry beyond it was never found at all. The
// regime is exactly rowCap x per-record cost > kWalkBudgetMs — with 64 rows,
// any store charging more than 6.25 ms a record, and lower still on a table
// with dead entries in it, which cost a read and fill no row. At the 30 ms a
// record measured on a 200-record store (platformio.ini), a pass reads
// fourteen. What that fourteen is not is a crossover: both per-record figures
// in this file were taken on a 200-record store, and nothing in this tree
// measures what a record costs on a table of ten, so the count a smaller table
// reads does not follow from either.
//
// Row collection now resumes across passes the way the sweep does, under the
// same two cursor rules (resumeCursor() and nextCursor() below), so a table in
// that regime is walked over several passes instead of the same prefix being
// re-read for ever — and rowCycleDone() below is what makes a list publishable
// only once it has been all the way round.
//
// What is bought for the bound: pathCount() is the whole table and is
// unaffected, no pass is long, and the node stays up.
//
// Other surfaces quote this figure as the gap between two drains of a ring.
// Which surfaces, and what else moves when it is retuned, is the registry in
// RingDrain.h — one copy, kept there because that is where the drain's own
// frames-per-gap arithmetic is derived. It was written out a second time here
// and the two disagreed on their first day about where that arithmetic lives,
// which is what a duplicated list of this kind always comes to. Retuning this
// figure means going through that registry.
//
// What those surfaces say about it is that it is a floor under the gap rather
// than a ceiling over it, and that stayed true when the removal was unbounded
// on top of it; now that the removal shares this budget it is closer to being
// the real figure, over by one record and one removal.
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
// 100 ms, which is three yields to a full budget: each lands at the first
// record at or past 100 ms *since the previous yield*, and a fourth would need
// the whole 400 ms, by which point the budget has already ended the pass. Since
// the previous yield and not since the start of the walk, because a yield costs
// a tick that is itself charged to the budget: at 30 ms a record the three land
// at 120, 241 and 362 ms of walk time rather than at 100, 200 and 300. Those
// three figures are driven in test_snapshot_walk rather than divided out here,
// which is how the divided-out version of this sentence was found to be wrong.
//
// So the core is given up several times per pass at every per-record cost this
// repository has measured — three yields at 30 ms a record, two at 162 — while
// a walk short enough to finish inside 100 ms never reaches one at all. It is
// bounded at the other end by the same arithmetic: three yields is the ceiling
// however cheap a record becomes, which is the property no count of records
// has.
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
// below), and it is worth asserting because nothing else would catch a repeat:
// core 1's idle task is not watched (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
// is unset in both sdkconfigs, while CPU0's is set), so loopTask's own
// subscription in setup() is the only thing that would notice.
constexpr uint32_t kWalkYieldMs = 100;

// The relation the choice above rests on. A cadence that does not fit strictly
// more than twice in a budget gives the core up at most once a pass: the second
// yield needs 2 x the interval to have elapsed, and elapsing the whole budget
// is a stop rather than a yield.
static_assert(kWalkYieldMs > 0 && kWalkYieldMs * 2 < kWalkBudgetMs,
              "the yield cadence must fire more than once inside one budget");

// The share of the gap between two pass starts that the pass itself may take.
//
// One rule, asked two ways. walkBudgetFitsInterval() below applies it to the
// figure this file *chooses* — the budget — and nextIntervalMs() applies it to
// the figure only the node can *measure*, what a pass actually cost. They are
// the same statement about the same thing: a task that spends more than a
// quarter of its window in this function is a task that has stopped forwarding
// for the other three quarters, and at the limit stops leaving any window at
// all.
constexpr uint32_t kPassShareDiv = 4;

// The relation the paragraph above rests on, for the caller to assert: a walk
// has to end well inside the interval its pass is gated on, or passes run back
// to back and the RNS task never reaches the sleep that is what lets loopTask
// run at all. A quarter is where the line is drawn — at 400 ms against 5000 ms
// the margin is 12.5x, and even at the limit a pass ends a quarter of the way
// into a window, plus the record and the removal in hand.
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
  return kWalkBudgetMs <= intervalMs / kPassShareDiv;
}

// How long to wait before the next pass, given what this one cost.
//
// What this is for, since the guard above looks like it covers it
// -------------------------------------------------------------
// walkBudgetFitsInterval() bounds the part of a pass this file chooses: the
// budget, static_asserted at the call site in every environment, so no build
// ships a budget that could fill its own window. What it cannot bound is what
// a pass *costs*, and the two are not the same number:
//
//   * The budget is consulted between records and never inside one, so a pass
//     costs the budget plus the record in hand. At the worst per-record figure
//     anywhere in this tree — 162 ms, microStore's own measurement on a
//     200-record store (FileStore.h) — that is 562 ms against a 1250 ms
//     quarter. Comfortable, and this function returns the configured interval
//     unchanged for it.
//   * passMayStop() may not end a pass that has dereferenced nothing. That is
//     the forward-progress guarantee, and its price is stated where it is
//     defined: a pass costs the prefix it steps over plus one record, not the
//     budget, if stepping ever stops being free. Stepping is free today and
//     test_typed_store_iterator pins it against the microStore this build
//     resolves — but it is a promise of a dependency pinned to a branch, and
//     the abandoned branch fix/snapshot-walk-starves-loop ran on a microStore
//     where stepping *did* load. A 50-position prefix at 30 ms a record is
//     1530 ms; the 87-path T-Beam at 162 ms is seconds.
//
// So the missing half is the measured one, and this is it. Nothing else in the
// pass watches what the pass came to, and the gate it feeds — "has
// SNAPSHOT_INTERVAL_MS passed since the *start* of the last pass" — becomes a
// no-op the moment a pass outlasts the interval: the gate is already open when
// the pass returns, the next one begins at once, and that is exactly the shape
// that took a T-Beam down at 7.5 s a walk.
//
// The rule
// --------
// A pass gets at most a quarter of the gap between two pass starts, the same
// quarter the budget is held to. Under that it changes nothing, which is the
// ordinary node; over it, the next pass is put kPassShareDiv times the
// measured cost out. Measured on the *last* pass rather than the worst since
// boot, so a node that recovers returns to its configured interval instead of
// carrying its worst moment for ever.
//
// The ceiling
// -----------
// `ceilingMs` is not decoration and the version of this idea on the abandoned
// branch did not have one: `max(walkMs * 4, SNAPSHOT_INTERVAL_MS)` would put a
// node whose passes cost a minute onto a four-minute interval, and a node that
// slow needs its cleanup more often than that, not less. The caller passes the
// slowest clock inside a pass — kStaleSweepMs, the minute between one dead-path
// sweep cycle and the next — because a pass carrying a minute's clock has to
// come round at least once a minute or that clock is not honoured at all.
//
// The trade the ceiling makes is worth stating: up to it, the gap is at least
// four times the pass; at it, a pass costing a quarter of the ceiling or more
// gets less than that, and one costing the whole ceiling is back to back
// again. That is not a bound this function can win — a node spending a minute
// per pass is broken in a way scheduling cannot fix — and the ceiling is where
// it stops making the node's cleanup worse in exchange for a margin it has
// already lost.
//
// Written as three comparisons rather than one min/max chain so that the
// multiply is only ever reached below the ceiling's own quarter, where it
// cannot overflow: the same reason walkBudgetFitsInterval() divides rather
// than multiplies.
constexpr uint32_t nextIntervalMs(uint32_t passCostMs, uint32_t intervalMs,
                                  uint32_t ceilingMs) {
  return ceilingMs < intervalMs                    ? intervalMs
       : passCostMs <= intervalMs / kPassShareDiv  ? intervalMs
       : passCostMs >= ceilingMs / kPassShareDiv   ? ceilingMs
                                                   : passCostMs * kPassShareDiv;
}

// Whether a pass that has spent its budget may stop here.
//
// One rule for both halves of a pass — the walk between two records, and the
// removal between two entries — so there is one clock, one budget and one
// definition of "out of time" over the whole of refreshSnapshots().
//
// `didSomething` is the forward-progress guarantee, and it is the whole reason
// this is a function rather than `elapsed >= budget` written twice. A pass has
// a prefix it does not want: positions already collected, positions the sweep
// has been through. Stepping over that prefix costs nothing today — microStore
// loads a record on dereference and not on ++, which test_typed_store_iterator
// pins at both the TypedStore and the file-store layer — so a pass always
// reaches its own cursor. If that ever stopped being true, a budget that could
// stop the walk inside the prefix would stop it *before* its cursor, on every
// pass, for ever: the cursor would never advance and the node would publish the
// same nothing until it was rebooted. That is not hypothetical. It is what the
// abandoned branch fix/snapshot-walk-starves-loop did on a bench node in
// September 2026 — "a pass pinned at position 15 of 87 for ever, publishing an
// empty list" — back when stepping did load.
//
// So the budget may not end a pass until the pass has dereferenced something.
// Worst case is then the prefix plus one record rather than the budget, which
// is the price of the guarantee, and it is only ever paid if the dependency
// regresses. Every pass moves at least one position of real work forward
// whatever a step comes to cost.
inline bool passMayStop(uint32_t elapsedMs, bool didSomething,
                        uint32_t budgetMs = kWalkBudgetMs) {
  return didSomething && elapsedMs >= budgetMs;
}

// What the walk should do with the position it is standing on.
enum class WalkStep : uint8_t {
  Row,           // read it: it fills a row, and is swept as well if a sweep is running
  Examine,       // read it for the sweep alone; the rows are full, so it is not rendered
  Skip,          // step past without touching the filesystem
  StopBudget,    // out of time — the pass ends here; nextCursor() says where each
                 // cursor picks up, which is not always here
  StopRowsFull,  // the rows are full and no sweep wants the rest of the table
};

// Everything the decision is made from. Positions are offsets into the table in
// iteration order; times are milliseconds since the walk started, so neither
// can wrap inside one pass.
struct WalkState {
  size_t   rowsCollected = 0;   // rows in the staging list, this cycle, not this pass
  size_t   rowCap        = 0;   // SNAPSHOT_MAX_PATHS
  size_t   rowPos        = 0;   // where row collection resumes; 0 = a fresh cycle
  bool     sweeping      = false;
  size_t   pos           = 0;   // where the walk is
  size_t   sweepPos      = 0;   // where the sweep is resuming from; 0 = a fresh cycle
  uint32_t elapsedMs     = 0;
  uint32_t budgetMs      = kWalkBudgetMs;
  bool     readAny       = false;  // a record has been dereferenced this pass
};

// How much of a pass's budget the rows may spend before the sweep gets the
// rest.
//
// Half each, and half is chosen because nothing measured says otherwise: the
// two halves of the walk are two unrelated jobs sharing one clock, and there is
// no figure in this tree that makes one worth more than the other. Expressed as
// a fraction of the budget rather than a constant of its own so that retuning
// kWalkBudgetMs moves it.
constexpr uint32_t rowShareMs(uint32_t budgetMs) { return budgetMs / 2; }

// Whether the rows have spent their share and the rest of this pass belongs to
// the sweep.
//
// Why the sweep needs a share at all
// ----------------------------------
// The walk serves positions in one order — the table's — so whichever cursor
// is in front is served first, and the budget is spent by the time the walk
// reaches the other. That is fine while the two cursors travel together, which
// they do for as long as both are ratcheting forward at the same stop position.
// It stops being fine the moment the row cycle closes: rows go back to the
// front of the table (a finished cycle returns their cursor to 0) and the sweep
// does not, so every pass afterwards spends its whole budget re-reading the
// front for rows and stops before it reaches the sweep's cursor, which then
// advances only in the one pass per row cycle where the rows catch up to it.
//
// On the synthetic 200-entry table at 30 ms a record — half of it dead entries,
// which is what the sweep is for — the cursor sat at 70 for five passes, then
// 76 for five, then 82: the first dead entry was reached on pass 35 and the
// cycle closed on pass 115, which is nine and a half minutes at five seconds a
// pass. With a share it is pass 10 and pass 23. And there is a knife edge
// inside the old behaviour: where reads per pass divide SNAPSHOT_MAX_PATHS
// exactly — sixteen of them, at 25 ms a record — the row half fills the cap
// precisely as the budget runs out, and the cursor stopped at 64 and stayed
// there until the per-record cost happened to jitter. A cleanup that never runs
// is not a slow cleanup.
//
// Only the figures *with* the share — pass 10, pass 23, and a knife-edge cursor
// that goes past 64 — are asserted, in test_snapshot_walk. The ones without it
// are not, and cannot be from this tree: the rules they were measured under are
// the ones the share replaced. They are recorded as what they are, measurements
// of the old behaviour reproduced once in review, and as why the share exists —
// not as anything this code can be checked against.
//
// The condition
// -------------
// Only when the sweep's cursor is *ahead* of the row cursor, because that is
// the only arrangement in which serving the rows first can leave the sweep
// nothing. With the sweep behind or level, the walk reaches its cursor first or
// at the same position and it is served by the same reads — taking half the
// budget off the rows there would slow both halves and fix nothing.
//
// What it guarantees, exactly: the sweep cursor advances on every sweeping
// pass, because once the share is spent every position below the sweep's cursor
// is stepped over for free and the budget is only half gone. The exception is a
// single record expensive enough to carry the pass from inside the share to
// past the budget, which no share can legislate against — the same "checked
// between records, never inside one" the budget itself is subject to.
//
// Where that boundary is depends on whether records cost the same, and the
// difference is a factor of two. A record is served for the rows only while the
// clock is still *under* the share, so the row half can hand over as late as
// rowShareMs + c, and the sweep's record is then read only if that is still
// under the budget:
//
//   * Uniform cost: the first record of the pass already spends the whole
//     share, so the walk hands over at c and reaches the sweep for any
//     c < kWalkBudgetMs. The exception is c >= 400 ms — a record costing the
//     whole budget by itself, as the paragraph above says.
//   * Mixed costs: cheap records creep the clock to one millisecond short of
//     the share, and one record over rowShareMs then lands the pass at or past
//     the budget. The exception starts at c > 200 ms, half the figure the
//     uniform reading gives.
//
// Mixed is the real case — a filesystem charges what it charges, and one store
// was measured at 93-162 ms for a record (FileStore.h) — so 200 ms is the
// margin to hold this against, and the worst per-record cost measured anywhere
// in this tree is 81 % of it rather than 40 %. Both boundaries are driven in
// test_snapshot_walk rather than reasoned about here.
inline bool rowShareSpent(const WalkState& w) {
  return w.sweeping && w.sweepPos > w.rowPos &&
         w.elapsedMs >= rowShareMs(w.budgetMs);
}

// Whether this position is one the row half wanted and the share denied it.
//
// The caller needs this and cannot infer it from the step: a denied position
// comes back as Skip or Examine, which are also what a position before the row
// cursor comes back as. It matters because a cycle the share cut short is a
// prefix in exactly the way a budget-stopped one is — the rows did not reach
// those positions, so the list is not whole and must not be published, and the
// row cursor must be left where the rows actually stopped rather than where the
// walk did. rowCycleDone() and nextCursor() below take both as `cutShort`.
inline bool rowShareDenied(const WalkState& w) {
  return w.rowsCollected < w.rowCap && w.pos >= w.rowPos && rowShareSpent(w);
}

// One position.
//
// Order matters and each step is here for a reason:
//
//  1. "Nothing left to do" is answered before the clock, so a pass that
//     finished what it came for is never recorded as having run out of time.
//     The difference is not cosmetic: only a budget stop writes the cursors.
//
//     It is decided on the rows being *full*, not on this position wanting no
//     row, and that distinction is the second thing the abandoned branch got
//     wrong. With a resuming row cursor, `!wantRow` is true at position 0 on
//     every pass that is continuing a part-built list — it wants no row *here*,
//     which is not the same as wanting no more rows at all. Stopping on it ends
//     a resuming pass before it reaches its own cursor, and the short list is
//     then treated as a finished one. That branch published 19 rows on a node
//     holding 87.
//  2. The budget is next, and so covers every position that follows —
//     including the skip below. It used to sit after the skip, where a pass
//     stepping over a long prefix could not be stopped at all, and after the
//     row decision, where it did not apply to the expensive half of the walk.
//     It cannot stop a pass that has read nothing yet (passMayStop() above).
//  3. Only then the cheap step-over: past a position neither half wants —
//     already collected, already swept, or wanted by the rows after they have
//     spent their share of this pass (rowShareSpent() above). That last one is
//     what makes the step-over reach the sweep's cursor rather than the budget.
inline WalkStep walkStep(const WalkState& w) {
  const bool rowsFull  = w.rowsCollected >= w.rowCap;
  const bool wantRow   = !rowsFull && w.pos >= w.rowPos && !rowShareSpent(w);
  const bool wantSweep = w.sweeping && w.pos >= w.sweepPos;
  if (rowsFull && !w.sweeping) return WalkStep::StopRowsFull;
  if (passMayStop(w.elapsedMs, w.readAny, w.budgetMs)) return WalkStep::StopBudget;
  if (!wantRow && !wantSweep)  return WalkStep::Skip;
  return wantRow ? WalkStep::Row : WalkStep::Examine;
}

// Whether the row list this pass has been building is a whole one.
//
// Two ways round, and only these two: the cap filled, or the walk reached the
// end of the table without anything cutting the row half short. Anything else
// is a valid prefix and nothing more, and a prefix must not be published — a
// panel showing four paths on a node holding eighty is worse than a panel
// showing the last whole list, because nothing on it says which it is. The
// count beside the list comes from the table's own size() and stays exact
// either way.
//
// `cutShort` is the budget ending the pass *or* the share ending the row half
// (rowShareDenied() above), and the two have to be one argument because they
// have the same consequence: positions the rows wanted and did not reach. The
// caller passes `ranOut || rowsCut`.
inline bool rowCycleDone(size_t rowsCollected, size_t rowCap, bool cutShort) {
  return rowsCollected >= rowCap || !cutShort;
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

// Where a cursor resumes at the top of a pass, given how big the table is now.
//
// One rule, both cursors. The walk carries two — where the sweep left off and
// where row collection left off — and the hazard is identical for each, so it
// is defined once and asked twice rather than written out again for the newer
// one.
//
// A cursor is an offset into a table that is mutated between passes, so it can
// be left pointing past the end of one: the caller drops up to
// SNAPSHOT_MAX_PATHS entries at the end of a pass, which can take the table out
// from under a cursor that was beyond them. Such a cursor is reached by no
// position, and nothing downstream corrects it — nextCursor() below only ever
// moves forwards, so on a table slow enough for the budget to end every pass it
// would keep returning that same stale value: the sweep would stay part-way
// through for ever, the cycle would never close, and the minute clock that
// starts the next one would never be stamped again. The row cursor stranded the
// same way would stop a list ever being finished, so nothing would be published
// at all.
//
// So a cursor at or past the end of the table is not a cursor, and its cycle
// starts again from the front. Asked before the caller decides whether it is
// sweeping, so the pass that finds it stale is the one that starts afresh.
inline size_t resumeCursor(size_t cursor, size_t tableSize) {
  return cursor < tableSize ? cursor : 0;
}

// Where the row cursor resumes, which is the rule above plus one condition of
// its own.
//
// The row cursor indexes a list as well as a table, and the two have to agree:
// the staging list holds exactly what the positions before the cursor
// contributed. The one thing that can part them is the pass throwing between
// the last row it pushed and the cursor being written back — Diag::guard
// catches it, and reading a record under memory pressure can cause it. The
// rows collected before the throw are then in the list with the cursor still
// behind them, so the next pass collects them a second time and publishes a
// list with duplicates in it, or one short of the table.
//
// `passWasClean` is the caller's record of whether the last pass got as far as
// writing the cursor back. When it did not, the cycle starts again from the
// front: cheaper than reasoning about which rows survived, and it is the same
// answer resumeCursor() gives a cursor it cannot trust.
//
// Here rather than at the call site because it is a rule about a cursor, and
// every other rule about these two cursors is in this file where the host can
// drive it. It was the one piece of that bookkeeping left in RnsTransport.cpp,
// which cannot be compiled for the host at all.
inline size_t resumeRowCursor(size_t cursor, size_t tableSize, bool passWasClean) {
  return passWasClean ? resumeCursor(cursor, tableSize) : 0;
}

// Where a cursor resumes on the next pass, given where this one stopped.
//
// One rule, both cursors again, and for the same reason: what the sweep needed
// of its cursor is exactly what row collection needs of its own.
//
// Forwards only, within a cycle. The budget can end the walk anywhere,
// including at a position *behind* one of the cursors — a sweeping pass steps
// over the prefix it has already examined and can be stopped in it, and a pass
// resuming a part-built row list steps over the rows it already has. Writing
// the stop position in unconditionally would drag the cursor back to the front
// and leave the tail of the table never reached, which is the exact bug a
// resuming cursor is added to avoid. Getting all the way round (`!cutShort`)
// closes the cycle instead: for the sweep that puts the next one a
// kStaleSweepMs out, and for the rows it is what makes the list publishable.
//
// `cutShort` and `stoppedAt` are each cursor's own, not the walk's. They are
// the same for the sweep, whose half ends where the walk ends; for the rows
// they are where row collection stopped and whether anything stopped it —
// which after the share is spent is not where the walk stopped, because the
// walk carries on to the sweep's cursor without them.
//
// Forwards only is a ratchet, and a ratchet has to be answerable to the thing
// it indexes: resumeCursor() above is what keeps it from ratcheting into a
// table that has since shrunk past it.
inline size_t nextCursor(size_t cursor, size_t stoppedAt, bool cutShort) {
  if (!cutShort) return 0;
  return stoppedAt > cursor ? stoppedAt : cursor;
}

} // namespace Rns
