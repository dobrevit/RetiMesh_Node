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


// RingDrain: the bound that keeps a flooded ring from holding the RNS task
// past the watchdog. The properties pinned here are the ones the fix is: a pass
// takes at most the cap, what it leaves stays in the ring and is drained by
// the following pass, the watchdog is fed before, during and after the work,
// and "the cap ended it" means the cap left something behind — not that the
// pass happened to take exactly its allowance from a ring that is now empty.
// Two more are here because a surface depends on them: the ring is asked "is
// there more" at most once a pass and only when the cap ended it, which is the
// whole of what the tcp_drain_capped counter is; and a handler that returns
// early — the radio drain's short-item path — still counts against the cap and
// does not end the drain.
//
// The firmware's ring is FreeRTOS's, which the host cannot have; the ring
// here is a deque with the same manners — receive() takes the item out and
// hands back a pointer the caller must return, at its own address.
#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <deque>
#include <vector>
#include "../../src/sys/RingDrain.h"

// ---------------------------------------------------------------------------
// A ring with the FreeRTOS one's contract: an item is out of the ring from
// the moment it is received, and stays borrowed until it is given back.
// ---------------------------------------------------------------------------
class FakeRing {
public:
  void post(int id) { _queued.push_back(id); }
  size_t waiting() const { return _queued.size(); }

  int* receive() {
    _receives++;
    if (_queued.empty()) return nullptr;
    // Every item gets its own live address, as the FreeRTOS ring gives out a
    // pointer into its own storage. One reused slot would have hidden a drain
    // that cached a stale item pointer: the value read back would still be the
    // right one, and only a real ring would have shown it was not. A deque
    // keeps the earlier addresses valid as later ones are appended.
    _borrowed.push_back(_queued.front());
    _queued.pop_front();
    _out++;
    return &_borrowed.back();
  }
  void give_back(int*) { _returned++; }

  // The helper's "is there more". Separate from waiting() so a test can read
  // the depth without being counted as the drain having asked.
  bool any_left() { _anyLeftAsks++; return !_queued.empty(); }

  size_t receives() const     { return _receives; }
  size_t out() const          { return _out; }        // items handed out
  size_t returned() const     { return _returned; }   // items given back
  size_t anyLeftAsks() const  { return _anyLeftAsks; }

private:
  std::deque<int> _queued;
  std::deque<int> _borrowed;      // stable addresses for items that are out
  size_t _receives     = 0;
  size_t _out          = 0;
  size_t _returned     = 0;
  size_t _anyLeftAsks  = 0;
};

// The test's stand-in for Sys::RingItem: the item goes back however the
// handler is left, including by a throw.
class Held {
public:
  Held(FakeRing& r, int* item) : _r(r), _item(item) {}
  ~Held() { _r.give_back(_item); }
  Held(const Held&) = delete;
  Held& operator=(const Held&) = delete;
private:
  FakeRing& _r;
  int*      _item;
};

// What a pass did, in the order it did it.
struct Record {
  std::vector<int>       handled;   // item ids, in order
  std::vector<size_t>    feedsAt;   // items completed when each feed happened
  std::vector<const int*> addrs;    // the item pointer the handler was given
};

// One pass with the plumbing the firmware uses: RAII return, a counting feed,
// and the ring's own item count as the "is there more" answer.
static Sys::RingDrain::Pass run(FakeRing& ring, Record& rec, size_t cap, size_t feedEvery) {
  return Sys::RingDrain::drain(
      cap, feedEvery,
      [&ring]() { return ring.receive(); },
      [&ring, &rec](int* item) {
        Held held(ring, item);
        rec.handled.push_back(*item);
        rec.addrs.push_back(item);
      },
      [&rec]() { rec.feedsAt.push_back(rec.handled.size()); },
      [&ring]() { return ring.any_left(); });
}

static void fill(FakeRing& ring, int count, int from = 0) {
  for (int i = 0; i < count; i++) ring.post(from + i);
}

static void test_the_constants_are_what_the_fix_committed_to() {
  // 64 is the value AutoInterface's kDrainBatch settled on, taken as the
  // precedent; 16 is the feed cadence, and refreshSnapshots()'s record walk now
  // reads it from here rather than keeping its own — so a retune of this one
  // moves the walk with it. Either should have to change this test to get past
  // review.
  TEST_ASSERT_EQUAL_size_t(64, Sys::RingDrain::kBatch);
  TEST_ASSERT_EQUAL_size_t(16, Sys::RingDrain::kFeedEvery);
}

// The Accept clause's own proof: more than the cap is queued, one pass takes
// exactly the cap, and the remainder is still there for the next pass.
static void test_more_than_the_cap_is_taken_a_batch_at_a_time() {
  FakeRing ring;
  fill(ring, 100);

  Record first;
  Sys::RingDrain::Pass p1 = run(ring, first, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p1.handled);
  TEST_ASSERT_TRUE(p1.capped);
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, first.handled.size());
  // Nothing was dropped on the way: the remainder is still in the ring.
  TEST_ASSERT_EQUAL_size_t(100 - Sys::RingDrain::kBatch, ring.waiting());

  // ...and the following pass — the next 10 ms tick — drains it.
  Record second;
  Sys::RingDrain::Pass p2 = run(ring, second, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(100 - Sys::RingDrain::kBatch, p2.handled);
  TEST_ASSERT_FALSE(p2.capped);
  TEST_ASSERT_EQUAL_size_t(0, ring.waiting());

  // Every item, once, in the order it was posted, across the two passes.
  TEST_ASSERT_EQUAL_size_t(100, first.handled.size() + second.handled.size());
  for (int i = 0; i < 100; i++) {
    const int got = i < (int)Sys::RingDrain::kBatch
                      ? first.handled[i]
                      : second.handled[i - (int)Sys::RingDrain::kBatch];
    TEST_ASSERT_EQUAL_INT(i, got);
  }
  // And every item that left the ring went back to it.
  TEST_ASSERT_EQUAL_size_t(100, ring.out());
  TEST_ASSERT_EQUAL_size_t(100, ring.returned());

  // Each item arrived at the handler on its own live address, as it does out of
  // a real ring. A drain that cached one item pointer and reused it would still
  // pass every assertion above.
  for (size_t i = 0; i < first.addrs.size(); i++)
    for (size_t j = i + 1; j < first.addrs.size(); j++)
      TEST_ASSERT_FALSE(first.addrs[i] == first.addrs[j]);

  // The ring was asked "is there more" once, by the pass the cap ended, and not
  // at all by the pass that ran it dry. See the dedicated test below for why
  // that matters to the counter.
  TEST_ASSERT_EQUAL_size_t(1, ring.anyLeftAsks());
}

// The tcp_drain_capped counter is only as honest as this: it fires on
// Pass::capped, and Pass::capped is whatever anyLeft() returned. Asking twice
// would double-count a pass; asking on a pass that ran the ring dry would race
// the producer into a "capped" that never happened. RingDrain.h states both;
// this is what holds it.
static void test_the_ring_is_asked_at_most_once_a_pass() {
  // Ran dry with room to spare: never asked.
  FakeRing shortRing;
  fill(shortRing, 10);
  Record shortRec;
  run(shortRing, shortRec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(0, shortRing.anyLeftAsks());

  // Empty from the start: never asked, and no watchdog lock taken to find out.
  FakeRing emptyRing;
  Record emptyRec;
  run(emptyRing, emptyRec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(0, emptyRing.anyLeftAsks());

  // The cap ended it with more waiting: asked exactly once.
  FakeRing over;
  fill(over, (int)Sys::RingDrain::kBatch * 3);
  Record overRec;
  Sys::RingDrain::Pass p = run(over, overRec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_TRUE(p.capped);
  TEST_ASSERT_EQUAL_size_t(1, over.anyLeftAsks());

  // The cap ended it with the ring now empty: still asked exactly once — the
  // asking is what tells those two apart — and the answer is what makes the
  // difference, not the number of asks.
  FakeRing exact;
  fill(exact, (int)Sys::RingDrain::kBatch);
  Record exactRec;
  Sys::RingDrain::Pass q = run(exact, exactRec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_FALSE(q.capped);
  TEST_ASSERT_EQUAL_size_t(1, exact.anyLeftAsks());
}

// The radio drain's short-item path, at the helper's level. There the handler
// counts g_stats.loraRxBadLength and returns; it was a `continue` in the loop
// this replaced. What has to survive that rewrite is the property `continue`
// gave for free: the handler's outcome must not decide whether the outer loop
// goes on. The literal mutation cannot happen in the lambda form — a bare
// `break` in the handler has no enclosing loop in that scope and would not
// compile — but a Handle grown a bool return that drain() then tested would
// stop the batch at the first bad item with the rest left behind, and every
// other test in this file would still pass.
static void test_a_handler_that_returns_early_still_counts_and_keeps_going() {
  FakeRing ring;
  fill(ring, (int)Sys::RingDrain::kBatch + 5);
  size_t early = 0, full = 0;
  Sys::RingDrain::Pass p = Sys::RingDrain::drain(
      Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery,
      [&ring]() { return ring.receive(); },
      [&ring, &early, &full](int* item) {
        Held held(ring, item);
        if (*item % 2 == 0) { early++; return; }   // the short-item path
        full++;
      },
      []() {},
      [&ring]() { return ring.any_left(); });

  // The pass ran its whole allowance, not as far as the first early return.
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p.handled);
  TEST_ASSERT_TRUE(p.capped);
  // Every item in the batch reached the handler, and an early return counts
  // against the cap like any other: the item left the ring either way.
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, early + full);
  TEST_ASSERT_EQUAL_size_t(32, early);
  TEST_ASSERT_EQUAL_size_t(32, full);
  // ...and went back to it.
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, ring.out());
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, ring.returned());
  // The remainder is untouched and drains on the next pass.
  TEST_ASSERT_EQUAL_size_t(5, ring.waiting());
}

static void test_a_flood_never_outruns_the_cap() {
  // The defect itself: a producer that refills the ring as fast as the drain
  // empties it. "Until the ring is empty" is then a condition the producer
  // decides, and a producer that decides never is what can hold the RNS task
  // past the watchdog (RingDrain.h, which also says that no such reset has
  // been observed and that the bench reproduction is outstanding). The flood
  // here is finite only so that a drain which lost its bound fails this test
  // instead of hanging it.
  FakeRing ring;
  fill(ring, 10);
  Record rec;
  size_t posted = 0;
  const size_t kFlood = 10000;
  Sys::RingDrain::Pass p = Sys::RingDrain::drain(
      Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery,
      [&ring, &posted]() {
        if (posted < kFlood) { ring.post(9999); posted++; }   // refilled under us
        return ring.receive();
      },
      [&ring, &rec](int* item) { Held held(ring, item); rec.handled.push_back(*item); },
      [&rec]() { rec.feedsAt.push_back(rec.handled.size()); },
      [&ring]() { return ring.any_left(); });
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p.handled);
  TEST_ASSERT_TRUE(p.capped);
  // It asked the ring exactly its allowance of times, not once per item the
  // flood produced.
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, ring.receives());
}

static void test_fewer_than_the_cap_drains_fully_and_is_not_capped() {
  FakeRing ring;
  fill(ring, 10);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(10, p.handled);
  TEST_ASSERT_FALSE(p.capped);
  TEST_ASSERT_EQUAL_size_t(0, ring.waiting());
  // One receive per item, plus the one that found the ring empty.
  TEST_ASSERT_EQUAL_size_t(11, ring.receives());
  // Nothing was deferred, so the ring was never asked whether anything was.
  TEST_ASSERT_EQUAL_size_t(0, ring.anyLeftAsks());
}

// The distinction the counter will be built on, in both directions.
static void test_exactly_the_cap_and_an_empty_ring_is_not_capped() {
  FakeRing ring;
  fill(ring, (int)Sys::RingDrain::kBatch);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p.handled);
  // Took its whole allowance and left nothing behind. A counter that fired
  // here would report a node being outrun when it had just kept up exactly.
  TEST_ASSERT_FALSE(p.capped);
}

static void test_exactly_the_cap_with_one_more_waiting_is_capped() {
  FakeRing ring;
  fill(ring, (int)Sys::RingDrain::kBatch + 1);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p.handled);
  // One item behind is the whole difference from the case above.
  TEST_ASSERT_TRUE(p.capped);
  TEST_ASSERT_EQUAL_size_t(1, ring.waiting());
}

static void test_the_feed_cadence_matches_the_snapshot_walk() {
  // refreshSnapshots(): feed before the walk, feed before every kFeedEvery-th
  // record, feed after the walk. Positions are the count of items completed
  // when the feed happened, so a feed "before the 16th" reads as 15.
  FakeRing ring;
  fill(ring, 100);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(Sys::RingDrain::kBatch, p.handled);

  const size_t expect[] = { 0, 15, 31, 47, 63, 64 };
  TEST_ASSERT_EQUAL_size_t(sizeof(expect) / sizeof(expect[0]), rec.feedsAt.size());
  for (size_t i = 0; i < rec.feedsAt.size(); i++)
    TEST_ASSERT_EQUAL_size_t(expect[i], rec.feedsAt[i]);

  // A short pass is bracketed the same way: before the first item, after the
  // last, and no cadence feed because it never reached one.
  FakeRing shortRing;
  fill(shortRing, 4);
  Record shortRec;
  run(shortRing, shortRec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  const size_t shortExpect[] = { 0, 4 };
  TEST_ASSERT_EQUAL_size_t(2, shortRec.feedsAt.size());
  for (size_t i = 0; i < shortRec.feedsAt.size(); i++)
    TEST_ASSERT_EQUAL_size_t(shortExpect[i], shortRec.feedsAt[i]);
}

static void test_an_empty_ring_is_a_no_op() {
  // Called a hundred times a second on a ring that is usually empty: one
  // receive, no feeds, no handler, nothing to report.
  FakeRing ring;
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(0, p.handled);
  TEST_ASSERT_FALSE(p.capped);
  TEST_ASSERT_EQUAL_size_t(1, ring.receives());
  TEST_ASSERT_EQUAL_size_t(0, rec.feedsAt.size());
  TEST_ASSERT_EQUAL_size_t(0, rec.handled.size());
  TEST_ASSERT_EQUAL_size_t(0, ring.anyLeftAsks());
}

static void test_a_zero_feed_cadence_does_not_divide_by_zero() {
  // Not a configuration anyone should write, but a modulo by zero is a crash
  // rather than a mistake, so it degrades to feeding on every item.
  FakeRing ring;
  fill(ring, 3);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, 0);
  TEST_ASSERT_EQUAL_size_t(3, p.handled);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3, (uint32_t)rec.feedsAt.size());
}

static void test_a_zero_cap_takes_nothing_and_reports_nothing() {
  // The other degenerate argument, and the one that would lie rather than
  // crash: with a zero cap the loop never runs, so the pass deferred nothing —
  // but the ring is full, so asking it would make a pass that did no work read
  // as capped, and the counter built on Pass::capped would climb on a node
  // nobody is outrunning. Neither caller can pass a zero cap; the cadence
  // beside it is defended for the same reason.
  FakeRing ring;
  fill(ring, 10);
  Record rec;
  Sys::RingDrain::Pass p = run(ring, rec, 0, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(0, p.handled);
  TEST_ASSERT_FALSE(p.capped);
  // Nothing taken, nothing asked, nothing reported, and the ring untouched.
  TEST_ASSERT_EQUAL_size_t(0, ring.receives());
  TEST_ASSERT_EQUAL_size_t(0, ring.anyLeftAsks());
  TEST_ASSERT_EQUAL_size_t(0, rec.feedsAt.size());
  TEST_ASSERT_EQUAL_size_t(10, ring.waiting());
}

static void test_a_throwing_handler_returns_its_item_and_keeps_the_count() {
  // incoming() runs a whole packet through microReticulum and can throw a
  // bad_alloc. The pass is abandoned — Diag::guard catches it a level up —
  // but the item it was holding goes back to the ring, and the items behind
  // it are still there for the pass after.
  FakeRing ring;
  fill(ring, 5);
  int seen = 0;
  bool threw = false;
  try {
    Sys::RingDrain::drain(
        Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery,
        [&ring]() { return ring.receive(); },
        [&ring, &seen](int* item) {
          Held held(ring, item);
          if (*item == 2) throw 42;      // the third item
          seen++;
        },
        []() {},
        [&ring]() { return ring.any_left(); });
  } catch (int) {
    threw = true;
  }
  TEST_ASSERT_TRUE(threw);
  TEST_ASSERT_EQUAL_INT(2, seen);
  // Three items left the ring and all three went back, the thrower included.
  TEST_ASSERT_EQUAL_size_t(3, ring.out());
  TEST_ASSERT_EQUAL_size_t(3, ring.returned());
  // The two behind it were never touched, and the next pass gets them.
  TEST_ASSERT_EQUAL_size_t(2, ring.waiting());

  Record after;
  Sys::RingDrain::Pass p = run(ring, after, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
  TEST_ASSERT_EQUAL_size_t(2, p.handled);
  TEST_ASSERT_FALSE(p.capped);
  TEST_ASSERT_EQUAL_INT(3, after.handled[0]);
  TEST_ASSERT_EQUAL_INT(4, after.handled[1]);
}

static void test_a_backlog_clears_in_as_many_passes_as_it_takes() {
  // A ring far deeper than one batch still empties, in ceil(n/cap) passes,
  // with nothing lost and nothing handled twice.
  FakeRing ring;
  const int kBacklog = 409;               // a backlog several batches deep
  fill(ring, kBacklog);
  size_t passes = 0, total = 0;
  Record rec;
  for (;;) {
    Sys::RingDrain::Pass p = run(ring, rec, Sys::RingDrain::kBatch, Sys::RingDrain::kFeedEvery);
    passes++;
    total += p.handled;
    if (!p.capped) break;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(20, (uint32_t)passes);   // a bound, so a stuck drain fails here
  }
  TEST_ASSERT_EQUAL_size_t(kBacklog, total);
  TEST_ASSERT_EQUAL_size_t(7, passes);              // ceil(409/64)
  TEST_ASSERT_EQUAL_size_t(0, ring.waiting());
  TEST_ASSERT_EQUAL_size_t(kBacklog, rec.handled.size());
  for (int i = 0; i < kBacklog; i++) TEST_ASSERT_EQUAL_INT(i, rec.handled[i]);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_constants_are_what_the_fix_committed_to);
  RUN_TEST(test_more_than_the_cap_is_taken_a_batch_at_a_time);
  RUN_TEST(test_a_flood_never_outruns_the_cap);
  RUN_TEST(test_fewer_than_the_cap_drains_fully_and_is_not_capped);
  RUN_TEST(test_exactly_the_cap_and_an_empty_ring_is_not_capped);
  RUN_TEST(test_exactly_the_cap_with_one_more_waiting_is_capped);
  RUN_TEST(test_the_ring_is_asked_at_most_once_a_pass);
  RUN_TEST(test_a_handler_that_returns_early_still_counts_and_keeps_going);
  RUN_TEST(test_the_feed_cadence_matches_the_snapshot_walk);
  RUN_TEST(test_an_empty_ring_is_a_no_op);
  RUN_TEST(test_a_zero_feed_cadence_does_not_divide_by_zero);
  RUN_TEST(test_a_zero_cap_takes_nothing_and_reports_nothing);
  RUN_TEST(test_a_throwing_handler_returns_its_item_and_keeps_the_count);
  RUN_TEST(test_a_backlog_clears_in_as_many_passes_as_it_takes);
  return UNITY_END();
}
