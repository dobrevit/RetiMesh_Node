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


// AutoIfPolicy: when the AutoInterface discovery multicast is worth sending.
// The properties pinned here are the ones the mesh depends on: the RNS
// cadence whenever anyone can hear it, minutes of grace after they leave, a
// back-off that stays inside the plan's band when they are gone, and an
// immediate answer — not a stale idle interval — the moment anyone appears.
#include <unity.h>
#include <stdint.h>
#include "../../src/rns/AutoIfPolicy.h"

static void test_the_constants_are_what_the_plan_committed_to() {
  // 1.6 s is RNS's own announce interval, not ours to retune; the idle
  // cadence must stay inside the plan's 10-30 s band and under the 22 s
  // peering timeout; the grace is "minutes". A silent retune of any of them
  // should have to change this test to get past review.
  TEST_ASSERT_EQUAL_UINT32(1600,   AutoIfPolicy::kActiveMs);
  TEST_ASSERT_EQUAL_UINT32(15000,  AutoIfPolicy::kIdleMs);
  TEST_ASSERT_EQUAL_UINT32(180000, AutoIfPolicy::kGraceMs);
}

static void test_the_first_ask_sends_even_alone_at_time_zero() {
  // A task that just started announces itself: its arrival is the news. And
  // it starts at the active cadence — the grace runs from here, so a node
  // that boots alone spends its first minutes discoverable at full rate.
  AutoIfPolicy p;
  TEST_ASSERT_TRUE(p.discoveryDue(0, false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kActiveMs, p.cadenceMs());
}

static void test_active_cadence_while_a_station_is_associated() {
  AutoIfPolicy p;
  TEST_ASSERT_TRUE(p.discoveryDue(0, true, false, 0));
  TEST_ASSERT_FALSE(p.discoveryDue(100, true, false, 0));
  TEST_ASSERT_FALSE(p.discoveryDue(1599, true, false, 0));
  TEST_ASSERT_TRUE(p.discoveryDue(1600, true, false, 0));
  // A due answer starts the next interval from itself.
  TEST_ASSERT_FALSE(p.discoveryDue(1700, true, false, 0));
  TEST_ASSERT_TRUE(p.discoveryDue(3200, true, false, 0));
}

// Presence is any of the three inputs alone; far past the grace, whichever
// one is still there keeps the cadence active.
static void expect_presence_kind_holds_active(bool ap, bool sta, size_t peers) {
  AutoIfPolicy p;
  TEST_ASSERT_TRUE(p.discoveryDue(0, ap, sta, peers));
  for (uint32_t t = 1600; t <= 2 * AutoIfPolicy::kGraceMs; t += 1600)
    TEST_ASSERT_TRUE(p.discoveryDue(t, ap, sta, peers));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kActiveMs, p.cadenceMs());
}
static void test_an_ap_station_alone_holds_the_active_cadence()  { expect_presence_kind_holds_active(true,  false, 0); }
static void test_a_sta_association_alone_holds_the_active_cadence() { expect_presence_kind_holds_active(false, true, 0); }
static void test_a_live_peer_alone_holds_the_active_cadence()    { expect_presence_kind_holds_active(false, false, 1); }

static void test_the_grace_holds_active_after_everyone_leaves_then_backs_off() {
  AutoIfPolicy p;
  TEST_ASSERT_TRUE(p.discoveryDue(0, true, false, 0));   // someone here at t=0
  // Gone from the next ask on: the active cadence holds for the whole grace.
  for (uint32_t t = 1600; t < AutoIfPolicy::kGraceMs; t += 1600) {
    TEST_ASSERT_TRUE(p.discoveryDue(t, false, false, 0));
    TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kActiveMs, p.cadenceMs());
  }
  const uint32_t lastActiveSend = AutoIfPolicy::kGraceMs - (AutoIfPolicy::kGraceMs % 1600); // 179200
  // The grace expires: idle cadence, and the last send starts the interval.
  TEST_ASSERT_FALSE(p.discoveryDue(AutoIfPolicy::kGraceMs, false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
  TEST_ASSERT_FALSE(p.discoveryDue(lastActiveSend + AutoIfPolicy::kIdleMs - 1, false, false, 0));
  TEST_ASSERT_TRUE(p.discoveryDue(lastActiveSend + AutoIfPolicy::kIdleMs, false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
}

// Deep in the idle back-off, mid-interval, somebody appears. The next ask
// must answer true right then — waiting out the rest of a 15 s interval is
// exactly the "re-peers within seconds" promise broken.
static void expect_instant_return(bool ap, bool sta, size_t peers) {
  AutoIfPolicy p;
  TEST_ASSERT_TRUE(p.discoveryDue(0, false, false, 0));            // boots alone
  const uint32_t idleAt = AutoIfPolicy::kGraceMs + AutoIfPolicy::kIdleMs;
  TEST_ASSERT_TRUE(p.discoveryDue(idleAt, false, false, 0));       // an idle-cadence send
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
  TEST_ASSERT_FALSE(p.discoveryDue(idleAt + 1000, false, false, 0));
  TEST_ASSERT_TRUE(p.discoveryDue(idleAt + 2000, ap, sta, peers)); // they appear: send now
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kActiveMs, p.cadenceMs());
  // And the active cadence runs from that send.
  TEST_ASSERT_FALSE(p.discoveryDue(idleAt + 2000 + 1599, ap, sta, peers));
  TEST_ASSERT_TRUE(p.discoveryDue(idleAt + 2000 + 1600, ap, sta, peers));
}
static void test_instant_return_when_an_ap_station_appears() { expect_instant_return(true,  false, 0); }
static void test_instant_return_when_the_sta_connects()      { expect_instant_return(false, true,  0); }
static void test_instant_return_on_the_first_peer_heard()    { expect_instant_return(false, false, 1); }

static void test_the_active_interval_holds_across_a_millis_wrap() {
  // millis() wraps every 49.7 days, which a solar node lives through many
  // times. Last sent just before the wrap; the asks just after are inside
  // the interval, and the ask exactly one interval later is due.
  AutoIfPolicy p;
  const uint32_t nearWrap = UINT32_MAX - 100;
  TEST_ASSERT_TRUE(p.discoveryDue(nearWrap, true, false, 0));
  TEST_ASSERT_FALSE(p.discoveryDue(UINT32_MAX, true, false, 0));   // 100 ms on
  TEST_ASSERT_FALSE(p.discoveryDue(1498, true, false, 0));         // 1599 ms on, wrapped
  TEST_ASSERT_TRUE(p.discoveryDue(1499, true, false, 0));          // exactly 1600
  // The grace straddling the wrap holds too: last present at 1499.
  TEST_ASSERT_TRUE(p.discoveryDue(1499 + 1600, false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kActiveMs, p.cadenceMs());
  TEST_ASSERT_TRUE(p.discoveryDue(1499 + AutoIfPolicy::kGraceMs, false, false, 0));
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
}

static void test_a_long_empty_stretch_stays_idle_across_the_wrap() {
  // Alone for over 49.7 days: when now - lastPresence wraps back under the
  // grace, the cadence must not drift to active for three minutes of
  // announcing to nobody. The policy pins the distance at the bound.
  AutoIfPolicy p;
  (void)p.discoveryDue(0, true, false, 0);                         // someone at boot
  (void)p.discoveryDue(AutoIfPolicy::kGraceMs, false, false, 0);   // long gone
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
  (void)p.discoveryDue(UINT32_MAX - 5000, false, false, 0);        // 49.7 days on
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
  (void)p.discoveryDue(100000, false, false, 0);                   // wrapped
  TEST_ASSERT_EQUAL_UINT32(AutoIfPolicy::kIdleMs, p.cadenceMs());
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_constants_are_what_the_plan_committed_to);
  RUN_TEST(test_the_first_ask_sends_even_alone_at_time_zero);
  RUN_TEST(test_active_cadence_while_a_station_is_associated);
  RUN_TEST(test_an_ap_station_alone_holds_the_active_cadence);
  RUN_TEST(test_a_sta_association_alone_holds_the_active_cadence);
  RUN_TEST(test_a_live_peer_alone_holds_the_active_cadence);
  RUN_TEST(test_the_grace_holds_active_after_everyone_leaves_then_backs_off);
  RUN_TEST(test_instant_return_when_an_ap_station_appears);
  RUN_TEST(test_instant_return_when_the_sta_connects);
  RUN_TEST(test_instant_return_on_the_first_peer_heard);
  RUN_TEST(test_the_active_interval_holds_across_a_millis_wrap);
  RUN_TEST(test_a_long_empty_stretch_stays_idle_across_the_wrap);
  return UNITY_END();
}
