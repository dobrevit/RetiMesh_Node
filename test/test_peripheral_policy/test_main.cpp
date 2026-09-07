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


// PeripheralPolicy: which parts are told what by the screen going dark and by
// the profile being switched. The rule is short and the reason to pin it is
// that it is the only thing standing between two register writes and a bus
// two tasks share: a verdict re-issued on every repeated event is a write on
// that bus for nothing, and a verdict lost across a profile change is a part
// left suspended with the screen lit — a compass that reports no heading, or
// a panel that stops following the hand, until something happens to blank it.
//
// What the register sequences themselves do is not testable here and is not
// faked: writing 0x00 to the magnetometer's control register, or clearing the
// accelerometer's enable bit, is an I2C transaction against a part that has to
// answer, and the only place that is proved is a bench.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/PeripheralPolicy.h"

// Power::Profile's ordinals. Written out rather than included, because
// Power.h is Arduino code and this test is not — and because a renumbering
// there ought to be a diff that has to be read twice.
static constexpr uint8_t kPerformance = 0;
static constexpr uint8_t kBalanced    = 1;
static constexpr uint8_t kBattery     = 2;
static constexpr uint8_t kProfiles[]  = { kPerformance, kBalanced, kBattery };

static constexpr bool kLit  = false;     // screenDark
static constexpr bool kDark = true;
static constexpr bool kScreens[] = { kLit, kDark };

using Verdict = PeripheralPolicy::Verdict;

// ---------------------------------------------------------------------------
// The rule, as pure arithmetic

// Both parts follow the screen, in every profile. This is the whole rule, and
// the exhaustive form of it is four lines because the domain is that small.
static void test_the_rule_is_the_screen_and_only_the_screen() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_TRUE(PeripheralPolicy::compassRuns(kLit, p));
    TEST_ASSERT_TRUE(PeripheralPolicy::imuRuns(kLit, p));
    TEST_ASSERT_FALSE(PeripheralPolicy::compassRuns(kDark, p));
    TEST_ASSERT_FALSE(PeripheralPolicy::imuRuns(kDark, p));
  }
}

// The profile is carried and deliberately not consulted. Pinned as an
// invariant rather than left to be inferred from the four assertions above:
// the day a profile is meant to move one of these, this is the test that has
// to be rewritten, which is the point of it being here.
static void test_the_profile_moves_neither_part() {
  for (bool dark : kScreens) {
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_EQUAL(PeripheralPolicy::compassRuns(dark, kPerformance),
                        PeripheralPolicy::compassRuns(dark, p));
      TEST_ASSERT_EQUAL(PeripheralPolicy::imuRuns(dark, kPerformance),
                        PeripheralPolicy::imuRuns(dark, p));
    }
  }
}

// The two questions are asked separately because they are two questions. That
// they agree today is a fact about what has been written so far, not about
// the shape of the rule.
static void test_both_parts_answer_alike_today() {
  for (bool dark : kScreens)
    for (uint8_t p : kProfiles)
      TEST_ASSERT_EQUAL(PeripheralPolicy::compassRuns(dark, p),
                        PeripheralPolicy::imuRuns(dark, p));
}

// ---------------------------------------------------------------------------
// What an event moves

// Both parts come up running, because begin() configures them and the screen
// is lit when it does. So the first broadcast after boot — Power::begin()
// applying the stored profile — must say nothing at all.
static void test_boot_says_nothing() {
  PeripheralPolicy pol;
  TEST_ASSERT_TRUE(pol.compassRunning());
  TEST_ASSERT_TRUE(pol.imuRunning());
  const PeripheralPolicy::Change c = pol.update(kLit, kPerformance);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
}

static void test_a_blank_suspends_both() {
  PeripheralPolicy pol;
  const PeripheralPolicy::Change c = pol.update(kDark, kPerformance);
  TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
}

// setBlank(true) reaches the same edge from the idle timer, the power menu, a
// long press and the deep-sleep path, and more than one of those can arrive
// with the panel already dark. Each re-issue would be two register writes on
// a bus another task is using.
static void test_a_blank_while_already_blanked_re_issues_nothing() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance);
  for (int i = 0; i < 5; i++) {
    const PeripheralPolicy::Change c = pol.update(kDark, kPerformance);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
  }
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
}

static void test_unblank_restores_both() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance);
  const PeripheralPolicy::Change c = pol.update(kLit, kPerformance);
  TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
  TEST_ASSERT_TRUE(pol.compassRunning());
  TEST_ASSERT_TRUE(pol.imuRunning());
}

static void test_a_wake_while_already_lit_re_issues_nothing() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance);
  (void)pol.update(kLit, kPerformance);
  for (int i = 0; i < 5; i++) {
    const PeripheralPolicy::Change c = pol.update(kLit, kPerformance);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
  }
}

// A handheld does this all day.
static void test_a_hundred_cycles_stay_in_step() {
  PeripheralPolicy pol;
  for (int i = 0; i < 100; i++) {
    PeripheralPolicy::Change c = pol.update(kDark, kBattery);
    TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
    c = pol.update(kLit, kBattery);
    TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
  }
  TEST_ASSERT_TRUE(pol.compassRunning());
  TEST_ASSERT_TRUE(pol.imuRunning());
}

// ---------------------------------------------------------------------------
// The profile arm of the broadcast

// The settings commit calls Power::apply() whether or not the profile changed,
// and apply() re-asks this policy. Lit or dark, that must write nothing.
static void test_a_profile_change_moves_nothing_either_way() {
  for (bool dark : kScreens) {
    PeripheralPolicy pol;
    (void)pol.update(dark, kPerformance);
    for (uint8_t p : kProfiles) {
      const PeripheralPolicy::Change c = pol.update(dark, p);
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
    }
    TEST_ASSERT_EQUAL(!dark, pol.compassRunning());
    TEST_ASSERT_EQUAL(!dark, pol.imuRunning());
  }
}

// The failure this shape exists to prevent: a profile switch while the screen
// is dark must not lose the fact that the parts are suspended, and the wake
// after it must still bring them back exactly once.
static void test_a_profile_change_while_dark_does_not_lose_the_state() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance);
  (void)pol.update(kDark, kBattery);
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
  PeripheralPolicy::Change c = pol.update(kLit, kBattery);
  TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
  c = pol.update(kLit, kBattery);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
}

// And the mirror of it: a screen change while a profile switch is the more
// recent event still moves both parts. Every ordering of the two inputs, in
// both directions, since the domain is small enough to walk.
static void test_every_ordering_of_the_two_inputs() {
  for (uint8_t first : kProfiles) {
    for (uint8_t second : kProfiles) {
      PeripheralPolicy pol;
      (void)pol.update(kLit, first);
      PeripheralPolicy::Change c = pol.update(kDark, first);
      TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
      c = pol.update(kDark, second);
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
      c = pol.update(kLit, second);
      TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_rule_is_the_screen_and_only_the_screen);
  RUN_TEST(test_the_profile_moves_neither_part);
  RUN_TEST(test_both_parts_answer_alike_today);
  RUN_TEST(test_boot_says_nothing);
  RUN_TEST(test_a_blank_suspends_both);
  RUN_TEST(test_a_blank_while_already_blanked_re_issues_nothing);
  RUN_TEST(test_unblank_restores_both);
  RUN_TEST(test_a_wake_while_already_lit_re_issues_nothing);
  RUN_TEST(test_a_hundred_cycles_stay_in_step);
  RUN_TEST(test_a_profile_change_moves_nothing_either_way);
  RUN_TEST(test_a_profile_change_while_dark_does_not_lose_the_state);
  RUN_TEST(test_every_ordering_of_the_two_inputs);
  return UNITY_END();
}
