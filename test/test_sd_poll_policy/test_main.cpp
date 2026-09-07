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
// waits it out, so there is no millis() wrap to test. The unbounded quantities
// are the two look counters, and that they saturate is tested instead.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/SdPollPolicy.h"

// The three numbers, spelled out here rather than read off the header, so a
// retune has to be made twice and shows up in the diff both times.
static void test_the_shipped_constants() {
  TEST_ASSERT_EQUAL_UINT32(3000,  SdPollPolicy::kBaseMs);
  TEST_ASSERT_EQUAL_UINT32(30000, SdPollPolicy::kAbsentCeilingMs);
  TEST_ASSERT_EQUAL_UINT32(30000, SdPollPolicy::kPresentMs);
  TEST_ASSERT_EQUAL_UINT32(30,    SdPollPolicy::kUnmountableHoldLooks);
  // The keep-alive is a tenfold slowdown on the beat the card used to be
  // touched at; if it ever equals the base again the change was a mistake.
  TEST_ASSERT_NOT_EQUAL(SdPollPolicy::kBaseMs, SdPollPolicy::kPresentMs);
  // The slice the caller feeds the watchdog on, and the one number the
  // watchdog reasoning in SdCard.cpp is written against.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, SdPollPolicy::kSliceMs);
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
  // Exactly kMaxRung, and that is the number the header names: rung n doubles
  // n-1 times, so the count stops one above kMaxDoublings — at the first count
  // that already produces the largest shift the clamp allows.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kMaxRung, settled);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kMaxDoublings + 1, SdPollPolicy::kMaxRung);
  for (uint32_t i = 0; i < 5000; i++) p.nextWaitMs(false, false);
  TEST_ASSERT_EQUAL_UINT32(settled, p.misses());
  // The shift cannot run away either, whatever count it is handed.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, SdPollPolicy::absentMs(UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs,
                           SdPollPolicy::absentMs(SdPollPolicy::kMaxRung));
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

static void test_a_card_that_will_not_mount_takes_the_base_beat_at_once() {
  // Unformatted, foreign filesystem, or a mount that failed: something is in
  // the slot and an operator may be about to format it. Whatever the empty
  // ladder had reached, the look after the card turns up is back on the base
  // beat, and the empty-slot count is dropped with it.
  SdPollPolicy p;
  for (int i = 0; i < 8; i++) p.nextWaitMs(false, false);          // backed off
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
  TEST_ASSERT_EQUAL_UINT32(1, p.unmountableLooks());
}

static void test_the_unmountable_hold_covers_someone_at_the_node() {
  // The whole hold is served at the base beat: a blank card pushed in and a
  // format asked for a minute later must not have to wait on a back-off. Thirty
  // looks of three seconds is a minute and a half.
  SdPollPolicy p;
  for (uint32_t look = 1; look <= SdPollPolicy::kUnmountableHoldLooks; look++) {
    TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));
    TEST_ASSERT_EQUAL_UINT32(look, p.unmountableLooks());
  }
  TEST_ASSERT_EQUAL_UINT32(90000, SdPollPolicy::kUnmountableHoldLooks * SdPollPolicy::kBaseMs);
}

static void test_an_unmountable_card_is_not_probed_every_three_seconds_for_ever() {
  // The finding this ladder exists for: `unformatted` and `error` are states an
  // operator lives with — a blank card, an ext4 card, a card left in a deployed
  // node — and holding the base beat for them for ever would spend the whole
  // saving on the one card nobody is coming back to. After the hold it climbs,
  // by the same doubling, to the same ceiling.
  SdPollPolicy p;
  for (uint32_t i = 0; i < SdPollPolicy::kUnmountableHoldLooks; i++) p.nextWaitMs(false, true);
  TEST_ASSERT_EQUAL_UINT32(6000,  p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(12000, p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(24000, p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, true));
  // And it stays there: a card left unmountable for a year does not creep past
  // the ceiling, and the counter it climbs on does not wrap back to the base.
  for (uint32_t i = 0; i < 20000; i++)
    TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, true));
  const uint32_t settled = p.unmountableLooks();
  for (uint32_t i = 0; i < 5000; i++) p.nextWaitMs(false, true);
  TEST_ASSERT_EQUAL_UINT32(settled, p.unmountableLooks());
}

static void test_the_unmountable_ladder_as_a_value() {
  // Read off statically, so the shape is written down once and a retune shows
  // up here as well as in the header. Domain from 1, like absentMs.
  const uint32_t hold = SdPollPolicy::kUnmountableHoldLooks;
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, SdPollPolicy::unmountableMs(1));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, SdPollPolicy::unmountableMs(hold));
  TEST_ASSERT_EQUAL_UINT32(6000,  SdPollPolicy::unmountableMs(hold + 1));
  TEST_ASSERT_EQUAL_UINT32(12000, SdPollPolicy::unmountableMs(hold + 2));
  TEST_ASSERT_EQUAL_UINT32(24000, SdPollPolicy::unmountableMs(hold + 3));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, SdPollPolicy::unmountableMs(hold + 4));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, SdPollPolicy::unmountableMs(UINT32_MAX));
  // The base beat is served for the hold and then never again: no rung after
  // the hold repeats it, which is what makes the climb a climb.
  for (uint32_t look = hold + 1; look < hold + 20; look++)
    TEST_ASSERT_TRUE(SdPollPolicy::unmountableMs(look) > SdPollPolicy::kBaseMs);
}

static void test_the_two_ladders_do_not_share_a_counter() {
  // A card that will not mount, then an empty slot: the unmountable climb must
  // not carry into the empty ladder, or a card pulled out of a node that had
  // been sitting with a blank one in it would be noticed on the ceiling rather
  // than on the base beat. And the reverse.
  SdPollPolicy p;
  for (uint32_t i = 0; i < SdPollPolicy::kUnmountableHoldLooks + 6; i++) p.nextWaitMs(false, true);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, true));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, false));   // card gone
  TEST_ASSERT_EQUAL_UINT32(0, p.unmountableLooks());
  TEST_ASSERT_EQUAL_UINT32(6000, p.nextWaitMs(false, false));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));    // back in
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
}

static void test_a_wake_puts_both_ladders_back_on_the_base_beat() {
  // The operator poke (SdCard::lookNow(), raised by the button). It is the only
  // thing other than the slot's own answer that resets the cadence, and it has
  // to work from either ladder's ceiling.
  SdPollPolicy p;
  for (uint32_t i = 0; i < 20; i++) p.nextWaitMs(false, false);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, p.nextWaitMs(false, false));
  p.wake();
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, false));
  TEST_ASSERT_EQUAL_UINT32(6000, p.nextWaitMs(false, false));   // and climbs again from there

  SdPollPolicy q;
  for (uint32_t i = 0; i < SdPollPolicy::kUnmountableHoldLooks + 10; i++) q.nextWaitMs(false, true);
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kAbsentCeilingMs, q.nextWaitMs(false, true));
  q.wake();
  TEST_ASSERT_EQUAL_UINT32(0, q.unmountableLooks());
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, q.nextWaitMs(false, true));
}

static void test_the_keep_alive_is_not_the_ceiling_wearing_its_number() {
  // kPresentMs and kAbsentCeilingMs are the same half minute today, so a policy
  // that had stopped reading `mounted` altogether would still answer the right
  // number once the empty ladder had settled. Discriminated without leaning on
  // the two being equal: from a fresh policy the empty ladder is at its base,
  // so only a policy that really looked at `mounted` can answer the keep-alive.
  SdPollPolicy p;
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, SdPollPolicy::absentMs(1));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kPresentMs, p.nextWaitMs(true, true));
  TEST_ASSERT_NOT_EQUAL(SdPollPolicy::absentMs(1), p.nextWaitMs(true, true));
  // The same for the unmountable answer, which shares its first rung with the
  // empty ladder but not its keep-alive.
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kBaseMs, p.nextWaitMs(false, true));
  TEST_ASSERT_NOT_EQUAL(SdPollPolicy::kPresentMs, p.nextWaitMs(false, true));
}

// The caller does not sleep an interval in one go: it serves it in slices and
// feeds the task watchdog between them. Every interval the policy can answer
// today is a whole number of slices, which makes the short final slice
// unreachable from the caller — so the arithmetic lives here and is pinned
// here, and a retune that stops dividing evenly still gets its remainder.
static void test_a_wait_is_sliced_into_whole_slices_and_a_remainder() {
  const uint32_t kBadlyChosen = 4000;      // not a multiple of the slice
  const uint32_t cases[] = { SdPollPolicy::kBaseMs, 6000, 24000,
                             SdPollPolicy::kAbsentCeilingMs, SdPollPolicy::kPresentMs,
                             kBadlyChosen, 1, 0 };
  for (uint32_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    const uint32_t ms = cases[c];
    uint32_t left = ms, slept = 0, slices = 0;
    while (left > 0) {
      const uint32_t slice = SdPollPolicy::nextSliceMs(left);
      TEST_ASSERT_TRUE(slice > 0);                            // or the loop never ends
      TEST_ASSERT_TRUE(slice <= SdPollPolicy::kSliceMs);      // or the watchdog waits too long
      left -= slice;
      slept += slice;
      slices++;
    }
    TEST_ASSERT_EQUAL_UINT32(ms, slept);                      // the whole wait, no more
    TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::slicesFor(ms), slices);
  }
  // The remainder case, spelled out: an interval that does not divide evenly
  // becomes whole slices plus a short one, and is not truncated or overrun.
  TEST_ASSERT_EQUAL_UINT32(2, SdPollPolicy::slicesFor(kBadlyChosen));
  TEST_ASSERT_EQUAL_UINT32(SdPollPolicy::kSliceMs, SdPollPolicy::nextSliceMs(kBadlyChosen));
  TEST_ASSERT_EQUAL_UINT32(1000, SdPollPolicy::nextSliceMs(kBadlyChosen - SdPollPolicy::kSliceMs));
  TEST_ASSERT_EQUAL_UINT32(0, SdPollPolicy::slicesFor(0));
  // And the slicing keeps the watchdog fed on the beat the task always used:
  // the longest thing the policy can ask for is ten of them.
  TEST_ASSERT_EQUAL_UINT32(10, SdPollPolicy::slicesFor(SdPollPolicy::kAbsentCeilingMs));
}

static void test_every_interval_clears_nothing_longer_than_the_ceiling() {
  // The caller slices the wait to keep the watchdog fed, and sizes the slice
  // on kBaseMs. Nothing the policy can answer may be shorter than that slice
  // or longer than the ceiling — the first would make the slicing pointless,
  // the second would put a wait outside what the documentation promises.
  SdPollPolicy p;
  bool sawBase = false, sawCeiling = false;
  for (uint32_t i = 0; i < 300; i++) {
    const bool mounted = (i % 7) == 0;
    const bool inSlot  = mounted || (i % 5) == 0;
    const uint32_t ms = p.nextWaitMs(mounted, inSlot);
    TEST_ASSERT_TRUE(ms >= SdPollPolicy::kBaseMs);
    TEST_ASSERT_TRUE(ms <= SdPollPolicy::kAbsentCeilingMs);
    if (ms == SdPollPolicy::kBaseMs) sawBase = true;
    if (ms == SdPollPolicy::kAbsentCeilingMs) sawCeiling = true;
  }
  // Both ends actually turned up. Without this the bounds above would be
  // satisfied by a policy that had stopped deciding anything and answered one
  // constant — which is not hypothetical, since the keep-alive and the ceiling
  // are the same number today.
  TEST_ASSERT_TRUE(sawBase);
  TEST_ASSERT_TRUE(sawCeiling);
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
  RUN_TEST(test_a_card_that_will_not_mount_takes_the_base_beat_at_once);
  RUN_TEST(test_the_unmountable_hold_covers_someone_at_the_node);
  RUN_TEST(test_an_unmountable_card_is_not_probed_every_three_seconds_for_ever);
  RUN_TEST(test_the_unmountable_ladder_as_a_value);
  RUN_TEST(test_the_two_ladders_do_not_share_a_counter);
  RUN_TEST(test_a_wake_puts_both_ladders_back_on_the_base_beat);
  RUN_TEST(test_the_keep_alive_is_not_the_ceiling_wearing_its_number);
  RUN_TEST(test_a_wait_is_sliced_into_whole_slices_and_a_remainder);
  RUN_TEST(test_every_interval_clears_nothing_longer_than_the_ceiling);
  return UNITY_END();
}
