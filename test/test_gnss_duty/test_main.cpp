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


// GnssDutyPolicy: when a receiver is allowed to stop looking.
//
// The failure this suite is really guarding against is not a receiver that
// sleeps too little. It is one that sleeps at the wrong moment — during the
// search, so that the search never finishes, or while somebody is watching a
// dial that then stops moving, or on a node nobody ever told what it is, which
// would make a firmware upgrade change behaviour on a fleet that asked for
// nothing.
//
// What the verdict is carried out *with* is not here and is not faked: a
// standby pin, a switched rail and a UBX message are three different pieces of
// hardware and the only place they are proved is a bench.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/GnssDutyPolicy.h"

using Verdict = GnssDutyPolicy::Verdict;

static constexpr uint8_t kUnset     = GnssDutyPolicy::kRoleUnset;
static constexpr uint8_t kCarried   = GnssDutyPolicy::kRoleCarried;
static constexpr uint8_t kTransport = GnssDutyPolicy::kRoleTransport;
static constexpr uint32_t kSettle   = GnssDutyPolicy::kSettleMs;
// The two roles that have a rest, for the tests that must hold for both.
static constexpr uint8_t kResting[] = { kCarried, kTransport };

// A node's state, spelled at each call site rather than mutated in place, so a
// test reads as the situation it describes.
static GnssDutyPolicy::State st(uint8_t role, bool fix, bool navLit = false) {
  GnssDutyPolicy::State s;
  s.role = role;
  s.fix = fix;
  s.navLit = navLit;
  return s;
}

// Runs the policy forward to `untilMs` at the rate the driver actually asks
// (ten times a second), and returns the last verdict. Stepping rather than
// jumping, because a policy that only works when it is asked at exactly the
// right instant is not one that works.
static Verdict run(GnssDutyPolicy& p, uint32_t& nowMs, uint32_t forMs,
                   const GnssDutyPolicy::State& s) {
  const uint32_t end = nowMs + forMs;
  Verdict v = p.update(nowMs, s);
  while ((int32_t)(end - nowMs) > 0) {
    nowMs += 100;
    v = p.update(nowMs, s);
  }
  return v;
}

// ---------------------------------------------------------------------------
// The role, and the fleet that never asked for any of this

// The whole upgrade-safety argument in one test. A node whose operator has
// never set a role behaves exactly as every node did before this file existed:
// the receiver tracks, for ever, fix or no fix, screen or no screen. If this
// test ever goes red, a firmware update has changed what deployed hardware
// does without anybody asking it to.
static void test_a_node_that_was_never_told_what_it_is_never_rests() {
  GnssDutyPolicy p;
  uint32_t now = 1000;
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 3600000, st(kUnset, i == 0)));
    TEST_ASSERT_FALSE(p.resting());
  }
}

// And so does every value this firmware does not recognise — a role written by
// a later build and read back by an older one, or a byte that NVS returned
// wrong. Unknown means "nobody decided", and the behaviour nobody decided is
// the one the node already had.
static void test_an_unrecognised_role_never_rests() {
  for (unsigned role = 3; role < 256; role++) {
    TEST_ASSERT_FALSE(GnssDutyPolicy::dutyCycles((uint8_t)role));
    GnssDutyPolicy p;
    uint32_t now = 1;
    TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 600000, st((uint8_t)role, true)));
  }
}

static void test_only_the_two_named_roles_duty_cycle() {
  TEST_ASSERT_FALSE(GnssDutyPolicy::dutyCycles(kUnset));
  TEST_ASSERT_TRUE(GnssDutyPolicy::dutyCycles(kCarried));
  TEST_ASSERT_TRUE(GnssDutyPolicy::dutyCycles(kTransport));
  TEST_ASSERT_EQUAL_UINT32(0, GnssDutyPolicy::restMsFor(kUnset));
  TEST_ASSERT_EQUAL_UINT32(GnssDutyPolicy::kRestCarriedMs, GnssDutyPolicy::restMsFor(kCarried));
  TEST_ASSERT_EQUAL_UINT32(GnssDutyPolicy::kRestTransportMs, GnssDutyPolicy::restMsFor(kTransport));
  // A carried node looks again sooner than a fixed one, because it moves.
  TEST_ASSERT_TRUE(GnssDutyPolicy::kRestCarriedMs < GnssDutyPolicy::kRestTransportMs);
  // And a rest is worth far more than the settle it costs, or the whole
  // exercise would be a receiver that never quite stops.
  TEST_ASSERT_TRUE(GnssDutyPolicy::kSettleMs * 4 < GnssDutyPolicy::kRestCarriedMs);
}

// ---------------------------------------------------------------------------
// You cannot duty-cycle a search

static void test_a_receiver_with_no_fix_is_never_rested() {
  for (uint8_t role : kResting) {
    GnssDutyPolicy p;
    uint32_t now = 5;
    TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 3600000, st(role, false)));
    TEST_ASSERT_FALSE(p.resting());
  }
}

// A fix that comes and goes never accumulates a settle: the clock starts again
// from each re-acquisition, so a receiver flickering in and out of lock under
// trees keeps looking rather than resting on the strength of one good second.
static void test_a_flickering_fix_never_settles() {
  GnssDutyPolicy p;
  uint32_t now = 0;
  for (int cycle = 0; cycle < 50; cycle++) {
    // Almost long enough, then lost.
    for (uint32_t t = 0; t < kSettle - 200; t += 100) {
      TEST_ASSERT_EQUAL(Verdict::Track, p.update(now, st(kTransport, true)));
      now += 100;
    }
    TEST_ASSERT_EQUAL(Verdict::Track, p.update(now, st(kTransport, false)));
    TEST_ASSERT_FALSE(p.settling());
    now += 100;
  }
  TEST_ASSERT_FALSE(p.resting());
}

// ---------------------------------------------------------------------------
// The ordinary cycle

static void test_a_transport_node_rests_once_its_fix_has_stood() {
  GnssDutyPolicy p;
  uint32_t now = 10000;
  const GnssDutyPolicy::State fixed = st(kTransport, true);
  // Not before the settle is up...
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, kSettle - 200, fixed));
  TEST_ASSERT_FALSE(p.resting());
  // ...and at it.
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, 400, fixed));
  TEST_ASSERT_TRUE(p.resting());
}

static void test_the_rest_lasts_the_role_s_own_interval() {
  for (uint8_t role : kResting) {
    GnssDutyPolicy p;
    uint32_t now = 1;
    const GnssDutyPolicy::State s = st(role, true);
    run(p, now, kSettle + 200, s);
    TEST_ASSERT_TRUE(p.resting());
    const uint32_t restMs = GnssDutyPolicy::restMsFor(role);
    // Still resting a tenth of a second before the interval is up.
    TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, restMs - 500, s));
    TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 1000, s));
  }
}

// The fix flag is deliberately ignored while resting: nothing arrives from a
// receiver that has been asked to stop, so "no fix" during a rest is the rest
// working. A policy that read it here would cancel every rest a tenth of a
// second after starting it.
static void test_the_fix_going_quiet_does_not_cancel_a_rest() {
  GnssDutyPolicy p;
  uint32_t now = 77;
  run(p, now, kSettle + 200, st(kTransport, true));
  TEST_ASSERT_TRUE(p.resting());
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, GnssDutyPolicy::kRestTransportMs - 1000,
                                       st(kTransport, false)));
}

// After a rest the receiver has to earn its next one from scratch — the settle
// starts again from the wake, not from whatever the fix was doing before the
// rest. Otherwise a receiver that never re-acquires would go on resting for
// ever on the strength of a fix it held ten minutes ago.
static void test_the_settle_starts_again_after_every_rest() {
  GnssDutyPolicy p;
  uint32_t now = 3;
  run(p, now, kSettle + 200, st(kTransport, true));
  TEST_ASSERT_TRUE(p.resting());
  // Forward to the end of the rest one pass at a time, so the instant of the
  // wake is known exactly rather than inferred.
  while (p.resting()) { now += 100; p.update(now, st(kTransport, true)); }
  const uint32_t woke = now;
  // A tenth of a second short of a *fresh* settle it is still tracking — even
  // though the fix it is holding never went away, and a policy that measured
  // the settle from before the rest would already have rested again.
  while (now - woke < kSettle - 200) {
    now += 100;
    TEST_ASSERT_EQUAL(Verdict::Track, p.update(now, st(kTransport, true)));
  }
  while (now - woke < kSettle + 400) { now += 100; p.update(now, st(kTransport, true)); }
  TEST_ASSERT_TRUE(p.resting());
}

// A receiver that wakes and never finds the sky again simply keeps looking.
// This is the self-correcting property the whole design leans on: a board
// whose hardware refuses the wake, or whose antenna has been snapped off,
// degrades to exactly today's behaviour instead of resting for ever.
static void test_a_receiver_that_never_reacquires_stays_awake() {
  GnssDutyPolicy p;
  uint32_t now = 1;
  run(p, now, kSettle + 200, st(kCarried, true));
  TEST_ASSERT_TRUE(p.resting());
  run(p, now, GnssDutyPolicy::kRestCarriedMs + 200, st(kCarried, false));
  TEST_ASSERT_FALSE(p.resting());
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 3600000, st(kCarried, false)));
  TEST_ASSERT_FALSE(p.resting());
}

// ---------------------------------------------------------------------------
// The screen somebody is looking at

static void test_a_lit_nav_screen_holds_a_carried_node_tracking() {
  GnssDutyPolicy p;
  uint32_t now = 500;
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 3600000, st(kCarried, true, true)));
  TEST_ASSERT_FALSE(p.resting());
}

// The same for a fixed node, and that is a deliberate choice rather than an
// oversight: a nav screen is only lit because somebody put it there, and a
// person standing in front of a relay reading its position wants a live one.
// A transport node's normal state — headless, or with the glass long since
// dark — is the case below it, and that one rests.
static void test_a_lit_nav_screen_holds_a_transport_node_tracking_too() {
  GnssDutyPolicy p;
  uint32_t now = 500;
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 600000, st(kTransport, true, true)));
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, kSettle + 200, st(kTransport, true, false)));
}

// A nav screen opened during a rest ends it on the next pass, not at the end
// of the interval. Waiting up to five minutes for the dial to come alive would
// make the feature read as a fault.
static void test_opening_a_nav_screen_ends_a_rest_at_once() {
  GnssDutyPolicy p;
  uint32_t now = 9;
  run(p, now, kSettle + 200, st(kTransport, true));
  TEST_ASSERT_TRUE(p.resting());
  run(p, now, 1000, st(kTransport, false));               // still resting
  TEST_ASSERT_TRUE(p.resting());
  TEST_ASSERT_EQUAL(Verdict::Track, p.update(now, st(kTransport, false, true)));
  TEST_ASSERT_FALSE(p.resting());
}

// Closing it does not immediately spend a rest either: the settle has to be
// earned again, so a walk through the GPS page and back does not leave the
// receiver napping on a fix from before the page was opened.
static void test_closing_a_nav_screen_earns_the_next_rest_afresh() {
  GnssDutyPolicy p;
  uint32_t now = 40000;
  run(p, now, 60000, st(kCarried, true, true));
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, kSettle - 300, st(kCarried, true, false)));
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, 600, st(kCarried, true, false)));
}

// ---------------------------------------------------------------------------
// Arithmetic that has to survive the fleet running for months

// millis() wraps every 49.7 days and these nodes are meant to run longer than
// that. Every comparison in the policy is on a difference rather than on two
// absolute stamps, and this is the test that says so: a rest that begins just
// before the wrap ends on time, rather than lasting another seven weeks.
static void test_a_rest_across_the_millis_wrap_ends_on_time() {
  GnssDutyPolicy p;
  uint32_t now = 0xFFFFFFFFu - (kSettle + 2000);
  run(p, now, kSettle + 200, st(kTransport, true));
  TEST_ASSERT_TRUE(p.resting());
  // Straight through zero.
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, GnssDutyPolicy::kRestTransportMs - 1000,
                                       st(kTransport, true)));
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, 2000, st(kTransport, true)));
}

// The one instant millis() reports zero is also this class's "no stamp yet".
// A settle begun at that instant must still complete rather than restarting on
// the next pass for ever.
static void test_a_fix_first_seen_at_time_zero_still_settles() {
  GnssDutyPolicy p;
  uint32_t now = 0;
  TEST_ASSERT_EQUAL(Verdict::Track, p.update(now, st(kCarried, true)));
  TEST_ASSERT_EQUAL(Verdict::Track, run(p, now, kSettle - 200, st(kCarried, true)));
  TEST_ASSERT_EQUAL(Verdict::Rest, run(p, now, 400, st(kCarried, true)));
}

// A long soak: a day of a fixed node in ordinary conditions. The verdict must
// keep alternating rather than latching either way, and the receiver must
// spend the great majority of that day switched off — which is the entire
// point of the milestone, stated as an assertion rather than as a hope.
static void test_a_day_of_a_fixed_node_is_mostly_rest() {
  GnssDutyPolicy p;
  uint32_t now = 12345;
  uint32_t tracking = 0, resting = 0, edges = 0;
  bool was = false;
  for (uint32_t t = 0; t < 24u * 3600u * 1000u; t += 100) {
    const Verdict v = p.update(now, st(kTransport, true));
    if (v == Verdict::Rest) resting += 100; else tracking += 100;
    if (p.resting() != was) { was = p.resting(); edges++; }
    now += 100;
  }
  TEST_ASSERT_TRUE(edges > 100);                       // it kept cycling
  TEST_ASSERT_TRUE(resting > tracking * 20);           // and mostly slept
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_node_that_was_never_told_what_it_is_never_rests);
  RUN_TEST(test_an_unrecognised_role_never_rests);
  RUN_TEST(test_only_the_two_named_roles_duty_cycle);
  RUN_TEST(test_a_receiver_with_no_fix_is_never_rested);
  RUN_TEST(test_a_flickering_fix_never_settles);
  RUN_TEST(test_a_transport_node_rests_once_its_fix_has_stood);
  RUN_TEST(test_the_rest_lasts_the_role_s_own_interval);
  RUN_TEST(test_the_fix_going_quiet_does_not_cancel_a_rest);
  RUN_TEST(test_the_settle_starts_again_after_every_rest);
  RUN_TEST(test_a_receiver_that_never_reacquires_stays_awake);
  RUN_TEST(test_a_lit_nav_screen_holds_a_carried_node_tracking);
  RUN_TEST(test_a_lit_nav_screen_holds_a_transport_node_tracking_too);
  RUN_TEST(test_opening_a_nav_screen_ends_a_rest_at_once);
  RUN_TEST(test_closing_a_nav_screen_earns_the_next_rest_afresh);
  RUN_TEST(test_a_rest_across_the_millis_wrap_ends_on_time);
  RUN_TEST(test_a_fix_first_seen_at_time_zero_still_settles);
  RUN_TEST(test_a_day_of_a_fixed_node_is_mostly_rest);
  return UNITY_END();
}
