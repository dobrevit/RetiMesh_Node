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
//  The bounded transmit queue of the UART byte-stream driver (issue 9).
//
//  Two of that entry's four acceptance clauses are about this class and both
//  are provable here rather than on a bench: that queue depth and drops are
//  counted, and that the queue never grows without bound. The third — no loss
//  at each supported baud on a good wire — needs two boards and a wire, and is
//  on the operator's checklist. The fourth, resynchronising on the next valid
//  frame after a mid-frame disconnection, belongs to HDLC::Deframer and is
//  already pinned by test_hdlc's corruption cases.
//
//  The ring arithmetic is where the bugs live, so the wrap cases are written
//  first and asserted byte for byte: a frame that straddles the end of the
//  arena and copied only its first half is the defect this shape exists to
//  make impossible, and a test that only checks lengths would not see it.
// ============================================================================
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "../../src/net/UartTxQueue.h"

static uint8_t   sArena[64];
static Uart::TxQueue::Slot sSlots[4];
static Uart::TxQueue q;

static void fresh() {
  q = Uart::TxQueue{};
  memset(sArena, 0, sizeof(sArena));
  q.attach(sArena, sizeof(sArena), sSlots, 4);
}

static bool pushPattern(uint8_t seed, size_t len) {
  uint8_t f[64];
  for (size_t i = 0; i < len; i++) f[i] = (uint8_t)(seed + i);
  return q.push(f, len);
}

static void expectPattern(uint8_t seed, size_t len) {
  uint8_t out[64]; size_t got = 0;
  TEST_ASSERT_TRUE(q.pop(out, sizeof(out), got));
  TEST_ASSERT_EQUAL_size_t(len, got);
  for (size_t i = 0; i < len; i++)
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(seed + i), out[i]);
}

void setUp() { fresh(); }
void tearDown() {}

// --- the basics ------------------------------------------------------------

static void test_a_frame_comes_back_exactly_as_it_went_in() {
  TEST_ASSERT_TRUE(pushPattern(0x10, 8));
  expectPattern(0x10, 8);
}

static void test_frames_come_back_in_the_order_they_went_in() {
  TEST_ASSERT_TRUE(pushPattern(0x10, 4));
  TEST_ASSERT_TRUE(pushPattern(0x40, 6));
  TEST_ASSERT_TRUE(pushPattern(0x70, 5));
  expectPattern(0x10, 4);
  expectPattern(0x40, 6);
  expectPattern(0x70, 5);
}

static void test_popping_an_empty_queue_says_so_rather_than_inventing_a_frame() {
  uint8_t out[8]; size_t got = 99;
  TEST_ASSERT_FALSE(q.pop(out, sizeof(out), got));
  TEST_ASSERT_EQUAL_UINT32(0, q.counters().framesSent);
}

// --- the bound, which is the acceptance clause -----------------------------

static void test_the_slot_count_bounds_the_queue() {
  for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(pushPattern((uint8_t)i, 2));
  // The fifth frame has arena room and no slot, and is refused.
  TEST_ASSERT_FALSE(pushPattern(0x99, 2));
  TEST_ASSERT_EQUAL_UINT16(4, q.depth());
  TEST_ASSERT_EQUAL_UINT32(1, q.counters().framesDropped);
}

static void test_the_arena_bounds_the_queue() {
  // Two 30-byte frames fit in 64; the third does not, though a slot is free.
  TEST_ASSERT_TRUE(pushPattern(0x10, 30));
  TEST_ASSERT_TRUE(pushPattern(0x40, 30));
  TEST_ASSERT_FALSE(pushPattern(0x70, 30));
  TEST_ASSERT_EQUAL_UINT16(2, q.depth());
  TEST_ASSERT_EQUAL_UINT32(1, q.counters().framesDropped);
}

static void test_pushing_for_ever_never_grows_the_queue() {
  // The clause in the entry's own words: no unbounded growth. A thousand
  // pushes against a queue of four, with pops only occasionally, and neither
  // the depth nor the bytes in use may exceed what was attached.
  for (int i = 0; i < 1000; i++) {
    pushPattern((uint8_t)i, 7);
    TEST_ASSERT_TRUE(q.depth() <= 4);
    TEST_ASSERT_TRUE(q.bytesUsed() <= sizeof(sArena));
    if (i % 3 == 0) { uint8_t o[64]; size_t n; q.pop(o, sizeof(o), n); }
  }
  TEST_ASSERT_TRUE(q.counters().framesDropped > 0);
}

static void test_a_refused_frame_is_counted_and_does_not_disturb_the_queue() {
  TEST_ASSERT_TRUE(pushPattern(0x10, 30));
  TEST_ASSERT_TRUE(pushPattern(0x40, 30));
  TEST_ASSERT_FALSE(pushPattern(0xF0, 30));     // refused
  // The frames already queued are untouched — the whole argument for dropping
  // the newest rather than evicting the oldest.
  expectPattern(0x10, 30);
  expectPattern(0x40, 30);
}

// --- the wrap, where ring bugs live ---------------------------------------

static void test_a_frame_that_straddles_the_end_of_the_arena_is_whole() {
  // The arithmetic is spelled out because it is the whole test: with a 64-byte
  // arena, two 25-byte frames put the write offset at 50, and a 20-byte frame
  // from there needs bytes 50..70 — which is 14 at the end and 6 at the start.
  //
  // Getting here needs the queue never to empty. An earlier version of this
  // test pushed and popped in pairs, which drains it, and a drained queue
  // restarts its write offset at zero — so it never wrapped and the test
  // passed on the strength of its name.
  TEST_ASSERT_TRUE(pushPattern(0x10, 25));       // [0,25)
  TEST_ASSERT_TRUE(pushPattern(0x40, 25));       // [25,50)
  expectPattern(0x10, 25);                       // frees [0,25); B still queued
  TEST_ASSERT_EQUAL_UINT16(1, q.depth());        // not empty, so offsets hold
  TEST_ASSERT_TRUE(pushPattern(0xA0, 20));       // [50,64) + [0,6) — the wrap
  expectPattern(0x40, 25);
  expectPattern(0xA0, 20);                       // every byte, across the seam
}

static void test_many_wraps_in_a_row_stay_byte_exact() {
  // A sliding window one frame deep, so the queue never drains and the write
  // offset keeps advancing: 200 frames of 15 bytes through a 64-byte arena is
  // forty-three wraps rather than none, counted by replaying the offsets.
  //
  // It has to slide rather than hold a frame back, because the queue is FIFO —
  // an earlier attempt parked one frame at the front and then popped expecting
  // the newest, which is not what a queue does, and the test failed for a
  // reason that was entirely its own.
  uint8_t prev = 1;
  TEST_ASSERT_TRUE(pushPattern(prev, 15));
  for (int i = 1; i < 200; i++) {
    const uint8_t seed = (uint8_t)(i * 7 + 1);
    TEST_ASSERT_TRUE(pushPattern(seed, 15));     // depth 2
    expectPattern(prev, 15);                     // pop the older; depth 1
    prev = seed;
  }
  expectPattern(prev, 15);
}

static void test_interleaved_push_and_pop_across_the_wrap_keeps_order() {
  // Two in flight rather than one, so the window is wider and the offsets
  // advance in uneven steps across the seam.
  uint8_t a = 1, b = 101;
  TEST_ASSERT_TRUE(pushPattern(a, 9));
  TEST_ASSERT_TRUE(pushPattern(b, 11));
  for (int round = 0; round < 100; round++) {
    const uint8_t na = (uint8_t)(a + 3), nb = (uint8_t)(b + 5);
    TEST_ASSERT_TRUE(pushPattern(na, 9));
    expectPattern(a, 9);
    TEST_ASSERT_TRUE(pushPattern(nb, 11));
    expectPattern(b, 11);
    a = na; b = nb;
    TEST_ASSERT_EQUAL_UINT16(2, q.depth());      // never drains, so it wraps
  }
  expectPattern(a, 9);
  expectPattern(b, 11);
}

static void test_a_drained_queue_restarts_at_the_beginning_of_the_arena() {
  // The behaviour the three tests above had to work around, asserted directly
  // so it is a decision rather than an accident: an empty queue reuses the
  // whole arena instead of creeping forward until it wraps for no reason.
  TEST_ASSERT_TRUE(pushPattern(0x10, 60));
  expectPattern(0x10, 60);
  TEST_ASSERT_EQUAL_size_t(0, q.bytesUsed());
  // If the offset had crept, a 60-byte frame would no longer fit in one piece
  // and the next push would have to wrap; it must simply succeed.
  TEST_ASSERT_TRUE(pushPattern(0x80, 60));
  expectPattern(0x80, 60);
}

static void test_a_frame_that_exactly_fills_the_remaining_bytes_is_admitted() {
  // The boundary, which nothing else pinned. Mutation testing found the byte
  // cap could be loosened by twenty-five bytes against a sixty-four byte arena
  // and every other case in this file still passed: the two that exercise it
  // overshoot by twenty-six, so they proved "grossly over is refused" and not
  // "the edge is here". This one fails the moment the comparison moves.
  TEST_ASSERT_TRUE(pushPattern(0x10, 64));        // exactly the arena
  TEST_ASSERT_EQUAL_UINT16(1, q.depth());
  TEST_ASSERT_EQUAL_size_t(64, q.bytesUsed());
  uint8_t one[1] = {0xFF};
  TEST_ASSERT_FALSE(q.push(one, 1));              // one byte over, refused
  expectPattern(0x10, 64);                        // and the frame is intact
}

static void test_a_drain_returns_every_byte_it_took() {
  // The clause "no unbounded growth" against the failure that actually causes
  // it — a byte-accounting leak on pop. The loop below asserts exact emptiness
  // rather than "within the cap", because a leak self-limits: once the leaked
  // total crosses the ceiling the same correct bound refuses everything after
  // it, so a test that only checks `bytesUsed() <= cap` cannot tell a queue
  // that is bounded from one that is stuck.
  for (int round = 0; round < 50; round++) {
    const size_t len = 3 + (size_t)(round % 13);
    for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(pushPattern((uint8_t)(round + i), len));
    uint8_t o[64]; size_t n;
    while (q.pop(o, sizeof(o), n)) {}
    TEST_ASSERT_EQUAL_UINT16(0, q.depth());
    TEST_ASSERT_EQUAL_size_t(0, q.bytesUsed());   // exactly, every round
  }
}

// --- counters --------------------------------------------------------------

static void test_the_high_water_mark_remembers_a_burst_the_depth_forgets() {
  for (int i = 0; i < 3; i++) pushPattern((uint8_t)i, 2);
  uint8_t o[8]; size_t n;
  while (q.pop(o, sizeof(o), n)) {}
  TEST_ASSERT_EQUAL_UINT16(0, q.depth());
  TEST_ASSERT_EQUAL_UINT16(3, q.counters().depthHighWater);
}

static void test_bytes_and_frames_are_counted_on_both_sides() {
  pushPattern(0x10, 4);
  pushPattern(0x20, 6);
  uint8_t o[64]; size_t n;
  q.pop(o, sizeof(o), n);
  const Uart::TxCounters& c = q.counters();
  TEST_ASSERT_EQUAL_UINT32(2, c.framesQueued);
  TEST_ASSERT_EQUAL_UINT32(10, c.bytesQueued);
  TEST_ASSERT_EQUAL_UINT32(1, c.framesSent);
  TEST_ASSERT_EQUAL_UINT32(4, c.bytesSent);
  TEST_ASSERT_EQUAL_UINT16(1, c.depth);
}

static void test_counters_survive_a_detach() {
  // A link switched off has not un-dropped what it dropped, and a surface
  // that showed the figure must not start lying because somebody toggled it.
  pushPattern(0x10, 4);
  for (int i = 0; i < 10; i++) pushPattern(0x20, 60);   // most refused
  const uint32_t dropped = q.counters().framesDropped;
  TEST_ASSERT_TRUE(dropped > 0);
  q.detach();
  TEST_ASSERT_EQUAL_UINT32(dropped, q.counters().framesDropped);
}

// --- refusing to be used wrongly ------------------------------------------

static void test_an_unattached_queue_refuses_and_counts_rather_than_crashing() {
  Uart::TxQueue idle;
  uint8_t f[4] = {1, 2, 3, 4};
  TEST_ASSERT_FALSE(idle.push(f, sizeof(f)));
  TEST_ASSERT_EQUAL_UINT32(1, idle.counters().framesDropped);
  uint8_t o[4]; size_t n;
  TEST_ASSERT_FALSE(idle.pop(o, sizeof(o), n));
}

static void test_a_zero_sized_arena_is_not_attached() {
  // The ring arithmetic takes a modulus by the capacity, so this would divide
  // by zero rather than refuse politely.
  Uart::TxQueue z;
  z.attach(sArena, 0, sSlots, 4);
  TEST_ASSERT_FALSE(z.attached());
  uint8_t f[2] = {1, 2};
  TEST_ASSERT_FALSE(z.push(f, sizeof(f)));
}

static void test_an_empty_frame_is_refused() {
  uint8_t f[1] = {0};
  TEST_ASSERT_FALSE(q.push(f, 0));
}

static void test_a_frame_too_large_for_the_readers_buffer_is_dropped_not_truncated() {
  // Half an HDLC frame on the wire is a corruption the far end must
  // resynchronise out of, which is worse than the frame never arriving.
  TEST_ASSERT_TRUE(pushPattern(0x10, 20));
  uint8_t small[8]; size_t n = 0;
  TEST_ASSERT_FALSE(q.pop(small, sizeof(small), n));
  TEST_ASSERT_EQUAL_UINT16(0, q.depth());          // consumed, not left stuck
  TEST_ASSERT_EQUAL_UINT32(1, q.counters().framesDropped);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_frame_comes_back_exactly_as_it_went_in);
  RUN_TEST(test_frames_come_back_in_the_order_they_went_in);
  RUN_TEST(test_popping_an_empty_queue_says_so_rather_than_inventing_a_frame);
  RUN_TEST(test_the_slot_count_bounds_the_queue);
  RUN_TEST(test_the_arena_bounds_the_queue);
  RUN_TEST(test_pushing_for_ever_never_grows_the_queue);
  RUN_TEST(test_a_refused_frame_is_counted_and_does_not_disturb_the_queue);
  RUN_TEST(test_a_frame_that_straddles_the_end_of_the_arena_is_whole);
  RUN_TEST(test_many_wraps_in_a_row_stay_byte_exact);
  RUN_TEST(test_interleaved_push_and_pop_across_the_wrap_keeps_order);
  RUN_TEST(test_a_drained_queue_restarts_at_the_beginning_of_the_arena);
  RUN_TEST(test_a_frame_that_exactly_fills_the_remaining_bytes_is_admitted);
  RUN_TEST(test_a_drain_returns_every_byte_it_took);
  RUN_TEST(test_the_high_water_mark_remembers_a_burst_the_depth_forgets);
  RUN_TEST(test_bytes_and_frames_are_counted_on_both_sides);
  RUN_TEST(test_counters_survive_a_detach);
  RUN_TEST(test_an_unattached_queue_refuses_and_counts_rather_than_crashing);
  RUN_TEST(test_a_zero_sized_arena_is_not_attached);
  RUN_TEST(test_an_empty_frame_is_refused);
  RUN_TEST(test_a_frame_too_large_for_the_readers_buffer_is_dropped_not_truncated);
  return UNITY_END();
}
