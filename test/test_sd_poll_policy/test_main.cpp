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


// SdPollPolicy: how long the card task waits between looks at the slot. The
// properties pinned here are the ones a wrong answer turns into either a
// wasted node — half a second of driver wait every three seconds for ever —
// or an operator standing in front of a node that will not admit the card
// they just pushed in.
//
// The policy holds no timestamp: it hands out an interval and the caller
// waits it out, so there is no millis() wrap to test. The unbounded quantity
// is the miss counter, and that it saturates is tested instead.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/SdPollPolicy.h"

// The three numbers, spelled out here rather than read off the header, so a
// retune has to be made twice and shows up in the diff both times.
static void test_the_shipped_constants() {
  TEST_ASSERT_EQUAL_UINT32(3000,  SdPollPolicy::kBaseMs);
  TEST_ASSERT_EQUAL_UINT32(30000, SdPollPolicy::kAbsentCeilingMs);
  TEST_ASSERT_EQUAL_UINT32(30000, SdPollPolicy::kPresentMs);
  // The keep-alive is a tenfold slowdown on the beat the card used to be
  // touched at; if it ever equals the base again the change was a mistake.
  TEST_ASSERT_NOT_EQUAL(SdPollPolicy::kBaseMs, SdPollPolicy::kPresentMs);
}

// The ladder, rung by rung, and the wall clock it adds up to. This is the
// promise the documentation makes to the operator: below the ceiling for the
// first three quarters of a minute, at it thereafter.
static void test_the_ladder_is_3_6_12_24_then_the_ceiling() {
  TEST_ASSERT_EQUAL_UINT32(3000,  SdPollPolicy::absentMs(1));
  TEST_ASSERT_EQUAL_UINT32(6000,  SdPollPolicy::absentMs(2));
  TEST_ASSERT_EQUAL_UINT32(12000, SdPollPolicy::absentMs(3));
  TEST_ASSERT_EQUAL_UINT32(24000, SdPollPolicy::absentMs(4));
  TEST_ASSERT_EQUAL_UINT32(30000, SdPollPolicy::absentMs(5));   // 48 s, clamped
  // 3 + 6 + 12 + 24 = 45 s of waiting before the fifth look, which is the
  // first one paced by the ceiling.
  TEST_ASSERT_EQUAL_UINT32(45000, SdPollPolicy::absentMs(1) + SdPollPolicy::absentMs(2) +
                                  SdPollPolicy::absentMs(3) + SdPollPolicy::absentMs(4));
}

static void test_the_first_empty_look_still_waits_the_old_three_seconds() {
  // A node that has just been switched on with nothing in the slot, or has
  // just had a card pulled out of it, behaves exactly as it always did for
  // the first look. The back-off is for the slot nobody is touching.
  SdPollPolicy p;
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, false));
  TEST_ASSERT_EQUAL_UINT32(1, p.misses());
}

static void test_the_interval_only_ever_grows_while_the_slot_stays_empty() {
  // Monotonic: no rung is shorter than the one before it, all the way to the
  // ceiling and well past it.
  SdPollPolicy p;
  uint32_t prev = 0;
  for (uint32_t look = 0; look < 200; look++) {
    const uint32_t ms = p.nextWaitMs(false, false);
    TEST_ASSERT_TRUE(ms >= prev);
    prev = ms;
  }
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, prev);
}

static void test_the_ceiling_is_reached_and_never_exceeded() {
  SdPollPolicy p;
  // Five looks to get there: 3, 6, 12, 24, then the ceiling.
  for (int i = 0; i < 4; i++) p.nextWaitMs(false, false);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, false));
  // And a slot left empty for a year does not creep past it.
  for (uint32_t i = 0; i < 10000; i++)
    TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, false));
}

static void test_the_miss_counter_saturates() {
  // The only unbounded quantity in here. It must stop climbing rather than
  // wrap: a wrapped counter would drop a node that has been empty for weeks
  // back onto the three-second beat and quietly undo the whole change.
  SdPollPolicy p;
  for (uint32_t i = 0; i < 5000; i++) p.nextWaitMs(false, false);
  const uint32_t settled = p.misses();
  TEST_ASSERT_TRUE(settled <= SdPollPolicy::kMaxDoublings + 1);
  for (uint32_t i = 0; i < 5000; i++) p.nextWaitMs(false, false);
  TEST_ASSERT_EQUAL_UINT32(settled, p.misses());
  // The shift cannot run away either, whatever count it is handed.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, SdPollPolicy::absentMs(UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs,
                           SdPollPolicy::absentMs(SdPollPolicy::kMaxDoublings + 1));
}

static void test_a_mounted_card_is_touched_on_the_keep_alive() {
  // Not the base, and not the absent ladder: the mounted card's own beat.
  SdPollPolicy p;
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kPresentMs, p.nextWaitMs(true, true));
  // A policy that ignored `mounted` would answer the base here, because a
  // mount resets the miss count — which is what makes this a real check.
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, SdPollPolicy::absentMs(p.misses()));
  // Mounted wins over the second argument, which it implies anyway.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kPresentMs, p.nextWaitMs(true, false));
}

static void test_a_mount_resets_the_ladder_to_the_base() {
  // The sequence that matters: an empty slot backs off, a card goes in and
  // mounts, the card is pulled out again. The look after the removal is back
  // on the three-second beat, because that is the moment somebody is most
  // likely to be putting it straight back in.
  SdPollPolicy p;
  for (int i = 0; i < 6; i++) p.nextWaitMs(false, false);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, false));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kPresentMs, p.nextWaitMs(true, true));   // mounted
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, false));    // removed
  TEST_ASSERT_EQUAL_UINT32(6000, p.nextWaitMs(false, false));                     // and off again
}

static void test_a_card_that_will_not_mount_keeps_the_base_beat() {
  // Unformatted, foreign filesystem, or a mount that failed: something is in
  // the slot and an operator is probably about to format it. Backing off here
  // would delay the format they asked for, and the expensive half-second wait
  // is not what this state costs anyway.
  SdPollPolicy p;
  for (int i = 0; i < 8; i++) p.nextWaitMs(false, false);          // backed off
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
  // And it stays there for as long as the card stays unmountable.
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));
}

static void test_every_interval_clears_nothing_longer_than_the_ceiling() {
  // The caller slices the wait to keep the watchdog fed, and sizes the slice
  // on kBaseMs. Nothing the policy can answer may be shorter than that slice
  // or longer than the ceiling — the first would make the slicing pointless,
  // the second would put a wait outside what the documentation promises.
  SdPollPolicy p;
  for (uint32_t i = 0; i < 300; i++) {
    const bool mounted = (i % 7) == 0;
    const bool inSlot  = mounted || (i % 5) == 0;
    const uint32_t ms = p.nextWaitMs(mounted, inSlot);
    TEST_ASSERT_TRUE(ms >= SdPollPolicy::kBaseMs);
    TEST_ASSERT_TRUE(ms <= SdPollPolicy::kAbsentCeilingMs);
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_shipped_constants);
  RUN_TEST(test_the_ladder_is_3_6_12_24_then_the_ceiling);
  RUN_TEST(test_the_first_empty_look_still_waits_the_old_three_seconds);
  RUN_TEST(test_the_interval_only_ever_grows_while_the_slot_stays_empty);
  RUN_TEST(test_the_ceiling_is_reached_and_never_exceeded);
  RUN_TEST(test_the_miss_counter_saturates);
  RUN_TEST(test_a_mounted_card_is_touched_on_the_keep_alive);
  RUN_TEST(test_a_mount_resets_the_ladder_to_the_base);
  RUN_TEST(test_a_card_that_will_not_mount_keeps_the_base_beat);
  RUN_TEST(test_every_interval_clears_nothing_longer_than_the_ceiling);
  return UNITY_END();
}
