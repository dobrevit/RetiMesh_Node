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

// Whether the screen is what reads the accelerometer — the board fact the IMU
// rule takes and the compass rule has no equivalent of. True on the two boards
// that had these parts first: one turns its panel from the accelerometer, the
// other levels a heading with it. False on the T-Beam Supreme, whose only
// readers are the console and the status API.
static constexpr bool kScreenReadsImu = true;
static constexpr bool kScreenDoesNot  = false;

// The magnetometer's own board fact, which is a different question: does a
// page show a bearing. True on the boards that had a compass first; false on
// the T-Beam Supreme, whose 128x64 pages show none and whose readers are the
// console and the status API. Named separately from the accelerometer's fact
// because passing one where the other belongs is the mistake these tests are
// here to fail on.
static constexpr bool kScreenReadsCompass = true;

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

// The two board facts arrive at update() as two types rather than two bools,
// so that swapping them does not compile — see PeripheralPolicy.h. These
// aliases keep the call sites below readable.
using ImuFact     = PeripheralPolicy::ImuFollowsScreen;
using CompassFact = PeripheralPolicy::CompassFollowsScreen;

// ---------------------------------------------------------------------------
// The rule, as pure arithmetic

// Both parts follow the screen, in every profile, where the screen is what
// reads them. This is the whole rule for the boards that had these parts
// first, and the exhaustive form of it is four lines because the domain is
// that small. What a board whose screen reads neither gets is two tests down.
static void test_the_rule_is_the_screen_and_only_the_screen() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_TRUE(PeripheralPolicy::compassRuns(kLit, p, kScreenReadsCompass));
    TEST_ASSERT_TRUE(PeripheralPolicy::imuRuns(kLit, p, kScreenReadsImu));
    TEST_ASSERT_FALSE(PeripheralPolicy::compassRuns(kDark, p, kScreenReadsCompass));
    TEST_ASSERT_FALSE(PeripheralPolicy::imuRuns(kDark, p, kScreenReadsImu));
  }
}

// The profile is carried and deliberately not consulted. Pinned as an
// invariant rather than left to be inferred from the four assertions above:
// the day a profile is meant to move one of these, this is the test that has
// to be rewritten, which is the point of it being here.
static void test_the_profile_moves_neither_part() {
  for (bool dark : kScreens) {
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_EQUAL(PeripheralPolicy::compassRuns(dark, kPerformance, kScreenReadsCompass),
                        PeripheralPolicy::compassRuns(dark, p, kScreenReadsCompass));
      TEST_ASSERT_EQUAL(PeripheralPolicy::imuRuns(dark, kPerformance, kScreenReadsImu),
                        PeripheralPolicy::imuRuns(dark, p, kScreenReadsImu));
      // and with the other board fact for each part, so a profile cannot
      // smuggle in a difference on the boards where the screen reads nothing
      TEST_ASSERT_EQUAL(PeripheralPolicy::imuRuns(dark, kPerformance, kScreenDoesNot),
                        PeripheralPolicy::imuRuns(dark, p, kScreenDoesNot));
      TEST_ASSERT_EQUAL(PeripheralPolicy::compassRuns(dark, kPerformance, kScreenDoesNot),
                        PeripheralPolicy::compassRuns(dark, p, kScreenDoesNot));
    }
  }
}

// The two questions are asked separately because they are two questions, and
// as of the T-Beam Supreme they no longer always agree.
// They answer alike on a board whose screen reads the accelerometer, which is
// what the original of this test asserted unconditionally. It is kept in that
// form, now qualified, because that agreement is still the case on both boards
// that carry the pair.
static void test_both_parts_answer_alike_where_the_screen_reads_the_imu() {
  for (bool dark : kScreens)
    for (uint8_t p : kProfiles)
      TEST_ASSERT_EQUAL(PeripheralPolicy::compassRuns(dark, p, kScreenReadsCompass),
                        PeripheralPolicy::imuRuns(dark, p, kScreenReadsImu));
}

// And they part company on a board whose screen does not. This is the case the
// header said would begin to check itself the day the two rules diverged:
// feeding the imu field from compassRuns() — the swap the header warns about —
// fails here and nowhere else.
static void test_the_imu_ignores_the_screen_where_the_screen_does_not_read_it() {
  for (bool dark : kScreens)
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_TRUE(PeripheralPolicy::imuRuns(dark, p, kScreenDoesNot));
      // and each part answers on its own fact: the compass still follows the
      // screen while a page shows a bearing, whatever the accelerometer's
      // consumer does
      TEST_ASSERT_EQUAL(!dark, PeripheralPolicy::compassRuns(dark, p, kScreenReadsCompass));
    }
}

// The same freedom for the magnetometer, which is what this board needed: a
// compass whose readers are a console and an API must not go dark with the
// glass. Before the fact existed the rule said !screenDark and there was
// nothing a board could do about it.
static void test_the_compass_ignores_the_screen_where_no_page_shows_a_bearing() {
  for (bool dark : kScreens)
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_TRUE(PeripheralPolicy::compassRuns(dark, p, kScreenDoesNot));
      TEST_ASSERT_EQUAL(!dark, PeripheralPolicy::imuRuns(dark, p, kScreenReadsImu));
    }
}

// The two facts are separate arguments and are not interchangeable. A caller
// that passed the accelerometer's fact to the magnetometer's question — or
// passed one of them twice, which is the shape of the mistake — gets a
// different answer here, on the mixed combination that the T-Beam Supreme is
// not and some future board might be.
static void test_the_two_facts_are_not_interchangeable() {
  for (uint8_t p : kProfiles) {
    // a board that displays a bearing but does not turn its panel from the
    // accelerometer: the compass sleeps with the glass, the accelerometer does
    // not — and swapping the arguments swaps exactly these two answers
    TEST_ASSERT_FALSE(PeripheralPolicy::compassRuns(kDark, p, kScreenReadsCompass));
    TEST_ASSERT_TRUE(PeripheralPolicy::imuRuns(kDark, p, kScreenDoesNot));
    TEST_ASSERT_TRUE(PeripheralPolicy::compassRuns(kDark, p, kScreenDoesNot));
    TEST_ASSERT_FALSE(PeripheralPolicy::imuRuns(kDark, p, kScreenReadsImu));
  }
}

// The whole point of the change, at the level the drivers see: a dark screen
// must not suspend a part whose only readers are the console and the API.
static void test_a_blank_leaves_that_imu_running_and_still_suspends_the_compass() {
  PeripheralPolicy pol;
  const PeripheralPolicy::Change c = pol.update(kDark, kPerformance, ImuFact{kScreenDoesNot}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_TRUE(pol.imuRunning());
}

// And it stays running however many times the screen moves, because a latch
// that answered on a count rather than on the value would show up here as an
// accelerometer that suspends on the second blank.
static void test_that_imu_survives_a_hundred_cycles_of_the_screen() {
  PeripheralPolicy pol;
  for (int i = 0; i < 100; i++) {
    const PeripheralPolicy::Change dk = pol.update(kDark, kPerformance, ImuFact{kScreenDoesNot}, CompassFact{kScreenReadsCompass});
    TEST_ASSERT_EQUAL(Verdict::Unchanged, dk.imu);
    TEST_ASSERT_TRUE(pol.imuRunning());
    const PeripheralPolicy::Change lt = pol.update(kLit, kPerformance, ImuFact{kScreenDoesNot}, CompassFact{kScreenReadsCompass});
    TEST_ASSERT_EQUAL(Verdict::Unchanged, lt.imu);
    TEST_ASSERT_TRUE(pol.imuRunning());
  }
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
  const PeripheralPolicy::Change c = pol.update(kLit, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
}

static void test_a_blank_suspends_both() {
  PeripheralPolicy pol;
  const PeripheralPolicy::Change c = pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
}

// setBlank(true) reaches the same edge from the idle timer, the power menu, a
// long press and the deep-sleep path, and more than one of those can arrive
// with the panel already dark. Each re-issue would be two register writes on
// a bus another task is using.
//
// A thousand of them, not a handful. The cycle test below alternates its
// input, so it can only prove that the latches stay in step; nothing there
// repeats an input, and a latch that decayed — or one that answered on a count
// rather than on the value — would be caught by neither five repeats nor any
// number of alternations. A dark node left dark is also the ordinary state of
// a deployed one: it is the case the latch has to survive for hours.
static void test_a_blank_while_already_blanked_re_issues_nothing() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  for (int i = 0; i < 1000; i++) {
    const PeripheralPolicy::Change c = pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
  }
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
}

static void test_unblank_restores_both() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  const PeripheralPolicy::Change c = pol.update(kLit, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
  TEST_ASSERT_TRUE(pol.compassRunning());
  TEST_ASSERT_TRUE(pol.imuRunning());
}

static void test_a_wake_while_already_lit_re_issues_nothing() {
  PeripheralPolicy pol;
  (void)pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  (void)pol.update(kLit, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  for (int i = 0; i < 5; i++) {
    const PeripheralPolicy::Change c = pol.update(kLit, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
  }
}

// A handheld does this all day. Alternating only — what a repeated input does
// is the test above's job.
static void test_a_hundred_cycles_stay_in_step() {
  PeripheralPolicy pol;
  for (int i = 0; i < 100; i++) {
    PeripheralPolicy::Change c = pol.update(kDark, kBattery, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
    TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
    TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
    c = pol.update(kLit, kBattery, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
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
    (void)pol.update(dark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
    for (uint8_t p : kProfiles) {
      const PeripheralPolicy::Change c = pol.update(dark, p, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
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
  (void)pol.update(kDark, kPerformance, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  (void)pol.update(kDark, kBattery, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_FALSE(pol.compassRunning());
  TEST_ASSERT_FALSE(pol.imuRunning());
  PeripheralPolicy::Change c = pol.update(kLit, kBattery, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
  TEST_ASSERT_EQUAL(Verdict::Run, c.compass);
  TEST_ASSERT_EQUAL(Verdict::Run, c.imu);
  c = pol.update(kLit, kBattery, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
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
      (void)pol.update(kLit, first, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
      PeripheralPolicy::Change c = pol.update(kDark, first, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
      TEST_ASSERT_EQUAL(Verdict::Suspend, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Suspend, c.imu);
      c = pol.update(kDark, second, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.compass);
      TEST_ASSERT_EQUAL(Verdict::Unchanged, c.imu);
      c = pol.update(kLit, second, ImuFact{kScreenReadsImu}, CompassFact{kScreenReadsCompass});
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
  RUN_TEST(test_both_parts_answer_alike_where_the_screen_reads_the_imu);
  RUN_TEST(test_the_imu_ignores_the_screen_where_the_screen_does_not_read_it);
  RUN_TEST(test_the_compass_ignores_the_screen_where_no_page_shows_a_bearing);
  RUN_TEST(test_the_two_facts_are_not_interchangeable);
  RUN_TEST(test_a_blank_leaves_that_imu_running_and_still_suspends_the_compass);
  RUN_TEST(test_that_imu_survives_a_hundred_cycles_of_the_screen);
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
