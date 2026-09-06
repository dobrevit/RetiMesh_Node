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


// ApIdlePolicy: when an empty access point is taken off the air. The
// properties pinned here are the ones a wrong verdict turns into a site
// visit: OFF means never, whatever the clock says; the window counts only an
// AP that is up and empty, from the moment it becomes so; the verdict
// latches — a down AP has no stations to end its own emptiness — and only a
// wake or the feature going off lifts it; and every interval holds across a
// millis() wrap.
#include <unity.h>
#include <stdint.h>
#include "../../src/net/ApIdlePolicy.h"

static const uint32_t kIdleMs = 10u * 60u * 1000u;   // ten minutes, the shipped default

static void test_disabled_never_suppresses_however_long_it_stands_empty() {
  // Default off (the ship decision): a node that has not opted in must
  // behave exactly as it always has, no matter how empty or how old.
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0, false, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs * 10, false, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(0xFFFFFFF0u, false, true, 0, kIdleMs));   // wrap-side too
  TEST_ASSERT_FALSE(p.suppressed(50, false, true, 0, kIdleMs));
}

static void test_the_first_ask_seeds_the_clock_not_the_epoch() {
  // A node whose first ask lands late in millis() must not read "empty since
  // zero" and take the AP down on the spot: the window starts when the
  // counting does.
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0x7000000u, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(0x7000000u + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(0x7000000u + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_the_boundary_is_exact_at_the_configured_minutes() {
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs - 1, true, true, 0, kIdleMs));   // one ms short
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs,     true, true, 0, kIdleMs));   // the window, exactly
}

static void test_counting_starts_when_the_last_station_leaves() {
  // Stations present hold the clock re-armed; the window runs from the last
  // ask that saw one (the caller asks every second, so that is the moment
  // of leaving to within a second), not from boot.
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0,     true, true, 2, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(60000, true, true, 1, kIdleMs));         // still someone
  TEST_ASSERT_FALSE(p.suppressed(61000, true, true, 0, kIdleMs));         // now empty
  TEST_ASSERT_FALSE(p.suppressed(60000 + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(60000 + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_a_station_appearing_resets_the_window() {
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs - 5000, true, true, 1, kIdleMs));  // saved at the bell
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs - 4000, true, true, 0, kIdleMs));  // and left again
  // The whole window runs again from the last ask that saw the station —
  // not the remainder of the old one.
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs * 2 - 5001, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs * 2 - 5000, true, true, 0, kIdleMs));
}

static void test_an_ap_that_is_not_up_does_not_count() {
  // Down for any other reason — still starting, held by a scan — re-arms:
  // the verdict is about beacons being sent for nobody, and none are.
  ApIdlePolicy p;
  TEST_ASSERT_FALSE(p.suppressed(0, true, false, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs * 3, true, false, 0, kIdleMs));    // hours down: nothing
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs * 3 + 1000, true, true, 0, kIdleMs));  // now up: counting
  // ... from the last down ask, which is when the emptiness became real.
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs * 4 - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs * 4,     true, true, 0, kIdleMs));
}

static void test_the_verdict_latches_while_the_ap_is_down() {
  // Once suppressed, the AP is beacon-less: no station can associate to end
  // the emptiness, so "ap down, zero stations" must not read as a re-arm.
  ApIdlePolicy p;
  p.suppressed(0, true, true, 0, kIdleMs);
  TEST_ASSERT_TRUE(p.suppressed(kIdleMs, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE(p.suppressed(kIdleMs + 1000, true, false, 0, kIdleMs));  // now actually down
  TEST_ASSERT_TRUE(p.suppressed(kIdleMs * 400, true, false, 0, kIdleMs));   // ... for days
}

static void test_a_wake_clears_the_verdict_at_once_and_rearms_the_clock() {
  ApIdlePolicy p;
  p.suppressed(0, true, true, 0, kIdleMs);
  TEST_ASSERT_TRUE(p.suppressed(kIdleMs, true, true, 0, kIdleMs));
  p.wake(kIdleMs + 5000);
  // Immediately, not on some later cadence — the 5 s return rides on this.
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs + 5000, true, true, 0, kIdleMs));
  // And the returning AP gets its whole window again, from the wake.
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs + 5000 + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs + 5000 + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_switching_the_feature_off_clears_the_latch() {
  // The operator turning the feature (or the AP's own switch — the caller
  // folds both into `enabled`) off must not find it re-enabled still
  // suppressed later: a policy asked to stand down stands down whole.
  ApIdlePolicy p;
  p.suppressed(0, true, true, 0, kIdleMs);
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs + 1000, false, true, 0, kIdleMs)); // off: lifted now
  // Back on: a fresh clock, not a stale verdict or a stale timestamp.
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs + 2000, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(kIdleMs + 2000 + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(kIdleMs + 2000 + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_the_window_holds_across_a_millis_wrap() {
  // Unsigned arithmetic: a window that starts just under the wrap must run
  // its full length through it, and no shorter or longer.
  ApIdlePolicy p;
  const uint32_t start = 0xFFFFFFF0u;                       // 16 ms before the wrap
  TEST_ASSERT_FALSE(p.suppressed(start, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(start + 1000, true, true, 0, kIdleMs));       // wrapped: 984
  TEST_ASSERT_FALSE(p.suppressed(start + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(start + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_a_wake_just_before_the_wrap_still_buys_the_whole_window() {
  ApIdlePolicy p;
  p.suppressed(0, true, true, 0, kIdleMs);
  p.suppressed(kIdleMs, true, true, 0, kIdleMs);            // latched
  const uint32_t wakeAt = 0xFFFFFFFAu;                      // 6 ms before the wrap
  p.wake(wakeAt);
  TEST_ASSERT_FALSE(p.suppressed(wakeAt, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(wakeAt + kIdleMs - 1, true, true, 0, kIdleMs));
  TEST_ASSERT_TRUE (p.suppressed(wakeAt + kIdleMs,     true, true, 0, kIdleMs));
}

static void test_a_live_shortening_of_the_window_moves_the_next_verdict() {
  // wifi.ap_idle_minutes applies live: the bound is passed per ask, so a
  // shorter window saved mid-count takes effect on the very next decision.
  ApIdlePolicy p;
  const uint32_t shortMs = 60u * 1000u;
  TEST_ASSERT_FALSE(p.suppressed(0, true, true, 0, kIdleMs));
  TEST_ASSERT_FALSE(p.suppressed(shortMs * 2, true, true, 0, kIdleMs));   // inside ten minutes
  TEST_ASSERT_TRUE (p.suppressed(shortMs * 2 + 1, true, true, 0, shortMs)); // over one minute
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_never_suppresses_however_long_it_stands_empty);
  RUN_TEST(test_the_first_ask_seeds_the_clock_not_the_epoch);
  RUN_TEST(test_the_boundary_is_exact_at_the_configured_minutes);
  RUN_TEST(test_counting_starts_when_the_last_station_leaves);
  RUN_TEST(test_a_station_appearing_resets_the_window);
  RUN_TEST(test_an_ap_that_is_not_up_does_not_count);
  RUN_TEST(test_the_verdict_latches_while_the_ap_is_down);
  RUN_TEST(test_a_wake_clears_the_verdict_at_once_and_rearms_the_clock);
  RUN_TEST(test_switching_the_feature_off_clears_the_latch);
  RUN_TEST(test_the_window_holds_across_a_millis_wrap);
  RUN_TEST(test_a_wake_just_before_the_wrap_still_buys_the_whole_window);
  RUN_TEST(test_a_live_shortening_of_the_window_moves_the_next_verdict);
  return UNITY_END();
}
