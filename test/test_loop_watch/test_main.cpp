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
//  When the stall watch speaks, on a host where the clock can be moved by hand.
//
//  The rule matters more than it looks. An alarm that fires on every slow
//  flash write is one an operator turns off, and one that says a node is stuck
//  and then never mentions it again is one they miss — and this is the alarm
//  that has to survive the case where the node cannot say anything else.
// ============================================================================
#include <unity.h>
#include "LoopWatch.h"

using namespace LoopWatch;

static void test_a_healthy_pass_says_nothing() {
  TEST_ASSERT_FALSE(stallDue(0, 0, false));
  TEST_ASSERT_FALSE(stallDue(200, 0, false));       // the loop's own delay
  TEST_ASSERT_FALSE(stallDue(kStallWarnMs - 1, 0, false));
}

static void test_a_stall_is_reported_once_it_passes_the_threshold() {
  TEST_ASSERT_TRUE(stallDue(kStallWarnMs, 0, false));
  TEST_ASSERT_TRUE(stallDue(kStallWarnMs * 10, 0, false));
}

static void test_it_does_not_repeat_itself_immediately() {
  // Said once, then quiet: a node stuck for an hour must not fill the log it
  // is being diagnosed from.
  TEST_ASSERT_FALSE(stallDue(kStallWarnMs * 2, 0, true));
  TEST_ASSERT_FALSE(stallDue(kStallWarnMs * 2, kStallRepeatMs - 1, true));
}

static void test_but_it_does_say_so_again_while_it_lasts() {
  // Once and never again is how a stall gets attributed to whatever was
  // happening at the moment it was first noticed, rather than to the state
  // the node is still in.
  TEST_ASSERT_TRUE(stallDue(kStallWarnMs * 2, kStallRepeatMs, true));
  TEST_ASSERT_TRUE(stallDue(kStallWarnMs * 2, kStallRepeatMs * 3, true));
}

static void test_coming_back_is_worth_a_line_of_its_own() {
  // A node that recovered by itself is a different problem from one that is
  // still stuck, and the log should be able to tell them apart afterwards.
  TEST_ASSERT_TRUE(recovered(true, 0));
  TEST_ASSERT_TRUE(recovered(true, kStallWarnMs - 1));
}

static void test_nothing_is_said_about_a_recovery_that_never_stalled() {
  TEST_ASSERT_FALSE(recovered(false, 0));
  TEST_ASSERT_FALSE(recovered(false, kStallWarnMs * 100));
}

static void test_a_node_still_stuck_has_not_recovered() {
  TEST_ASSERT_FALSE(recovered(true, kStallWarnMs));
  TEST_ASSERT_FALSE(recovered(true, kStallWarnMs * 10));
}

static void test_the_thresholds_are_the_right_way_round() {
  // The repeat has to be longer than the trigger, or a stall reports itself
  // continuously from the moment it is noticed.
  TEST_ASSERT_TRUE(kStallRepeatMs > kStallWarnMs);
  // And the trigger has to be well above one pass of the loop, which delays
  // 200 ms of its own accord.
  TEST_ASSERT_TRUE(kStallWarnMs > 1000);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_pass_says_nothing);
  RUN_TEST(test_a_stall_is_reported_once_it_passes_the_threshold);
  RUN_TEST(test_it_does_not_repeat_itself_immediately);
  RUN_TEST(test_but_it_does_say_so_again_while_it_lasts);
  RUN_TEST(test_coming_back_is_worth_a_line_of_its_own);
  RUN_TEST(test_nothing_is_said_about_a_recovery_that_never_stalled);
  RUN_TEST(test_a_node_still_stuck_has_not_recovered);
  RUN_TEST(test_the_thresholds_are_the_right_way_round);
  return UNITY_END();
}
