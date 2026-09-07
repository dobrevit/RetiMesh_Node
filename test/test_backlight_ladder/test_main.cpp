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


// BacklightLadder: how bright the glass is, given what it is showing. The
// reason to pin this rather than judge it by eye is that every one of its
// mistakes is invisible until somebody is standing in front of the board with
// a discharged cell: a ladder that dims the screen a person is reading gets
// the brightness turned up and costs more than it saved; one that dims the
// idle clock into darkness is a blank screen that still draws current; one
// that lets a step climb above the operator's setting has quietly overridden
// them.
//
// What the percentage then does to hardware is not tested here and is not
// faked: it is a duty cycle on one board and a count of pulses into a
// one-wire dimmer on another, and both are transactions against a panel that
// has to answer. The only place those are proved is a bench.
#include <unity.h>
#include <stdint.h>
#include "../../src/ui/BacklightLadder.h"

using Stage = BacklightLadder::Stage;

// Power::Profile's ordinals. Written out rather than included, because
// Power.h is Arduino code and this test is not — and because a renumbering
// there ought to be a diff that has to be read twice.
static constexpr uint8_t kPerformance = 0;
static constexpr uint8_t kBalanced    = 1;
static constexpr uint8_t kBattery     = 2;
static constexpr uint8_t kProfiles[]  = { kPerformance, kBalanced, kBattery };

// The shipped default (Settings.h) and the funnel's floor (Config.h), which
// are the two configured values a node in the field actually sits at.
static constexpr uint8_t kDefaultPct = 80;
static constexpr uint8_t kFloorPct   = BRIGHTNESS_FLOOR_PCT;

static constexpr uint8_t rung(uint8_t pct, Stage s, uint8_t profile) {
  return BacklightLadder::level(pct, s, profile);
}

// ---------------------------------------------------------------------------
// The ceiling

// The operator's setting is what a screen being read is lit at, whatever the
// profile says. This is the half of the rule that is a promise to the person
// holding the board, not a power saving.
static void test_the_working_screen_is_the_configured_percentage() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_EQUAL_UINT8(kDefaultPct, rung(kDefaultPct, Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(kFloorPct,   rung(kFloorPct,   Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(100,         rung(100,         Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(37,          rung(37,          Stage::Active, p));
  }
}

// No rung may climb above it — including the floored one, which is the only
// rung with a number of its own and so the only one that could.
static void test_no_stage_ever_exceeds_the_setting() {
  for (uint8_t pct = 0; pct <= 100; pct++)
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_LESS_OR_EQUAL_UINT8(pct, rung(pct, Stage::Active, p));
      TEST_ASSERT_LESS_OR_EQUAL_UINT8(pct, rung(pct, Stage::Idle, p));
      TEST_ASSERT_LESS_OR_EQUAL_UINT8(pct, rung(pct, Stage::Blank, p));
    }
}

// ---------------------------------------------------------------------------
// The dim step, which is what this file was added for

// Swept rather than checked at the shipped default: a step that only exists
// at 80 % is not a step, and the interesting end is the bottom, where the
// floor climbs up towards the ceiling and the two could meet early. From
// floor + 1 upwards there must always be somewhere left to dim to — at the
// floor itself there is not, which is its own case below.
static void test_the_idle_clock_is_dimmer_than_the_working_screen() {
  for (uint8_t pct = kFloorPct + 1; pct <= 100; pct++)
    for (uint8_t p : kProfiles)
      TEST_ASSERT_LESS_THAN_UINT8(rung(pct, Stage::Active, p),
                                  rung(pct, Stage::Idle, p));
}

// The constants themselves, at the shipped default: a retune is a diff on
// these three numbers rather than a panel somebody notices is different.
static void test_the_idle_rungs_are_the_numbers_they_are() {
  TEST_ASSERT_EQUAL_UINT8(40, rung(kDefaultPct, Stage::Idle, kPerformance));
  TEST_ASSERT_EQUAL_UINT8(20, rung(kDefaultPct, Stage::Idle, kBalanced));
  TEST_ASSERT_EQUAL_UINT8(10, rung(kDefaultPct, Stage::Idle, kBattery));
  // And the shifts they come from, so a table edited in the wrong row is
  // caught here and not only through the arithmetic above.
  TEST_ASSERT_EQUAL_UINT8(1, BacklightLadder::kIdleShift[kPerformance]);
  TEST_ASSERT_EQUAL_UINT8(2, BacklightLadder::kIdleShift[kBalanced]);
  TEST_ASSERT_EQUAL_UINT8(3, BacklightLadder::kIdleShift[kBattery]);
}

// ---------------------------------------------------------------------------
// Which rung is being asked for

// stageOf() is the other half of the rule, and it is pinned exhaustively
// because it has only eight inputs and every one of them is a screen somebody
// ends up looking at. The precedence is what matters: blank beats everything,
// the idle rung needs a shell that is actually running, and a board whose
// shell failed to start has no idle stage at all — it goes straight from the
// working screen to dark, which is exactly what its mono pages do.
static constexpr uint8_t stage(bool blank, bool shellUp, bool idleShowing) {
  return (uint8_t)BacklightLadder::stageOf(blank, shellUp, idleShowing);
}
static constexpr uint8_t kActive = (uint8_t)Stage::Active;
static constexpr uint8_t kIdle   = (uint8_t)Stage::Idle;
static constexpr uint8_t kBlank  = (uint8_t)Stage::Blank;

static void test_the_stage_is_read_off_the_three_flags() {
  //                                   blank  shell  idle
  TEST_ASSERT_EQUAL_UINT8(kActive, stage(false, false, false));
  TEST_ASSERT_EQUAL_UINT8(kActive, stage(false, false, true));   // no shell, no idle stage
  TEST_ASSERT_EQUAL_UINT8(kActive, stage(false, true,  false));
  TEST_ASSERT_EQUAL_UINT8(kIdle,   stage(false, true,  true));   // the only idle case
  TEST_ASSERT_EQUAL_UINT8(kBlank,  stage(true,  false, false));
  TEST_ASSERT_EQUAL_UINT8(kBlank,  stage(true,  false, true));
  TEST_ASSERT_EQUAL_UINT8(kBlank,  stage(true,  true,  false));
  TEST_ASSERT_EQUAL_UINT8(kBlank,  stage(true,  true,  true));   // blank outranks the clock
}

// And it folds, like the rest of the ladder: the display pass asks both
// halves once per pass and neither costs it a call.
static void test_the_stage_is_a_constant_expression() {
  static_assert(BacklightLadder::stageOf(true, true, true) == Stage::Blank, "");
  static_assert(BacklightLadder::stageOf(false, true, true) == Stage::Idle, "");
  static_assert(BacklightLadder::stageOf(false, true, false) == Stage::Active, "");
  constexpr uint8_t folded =
      BacklightLadder::level(80, BacklightLadder::stageOf(false, true, true), kBalanced);
  TEST_ASSERT_EQUAL_UINT8(20, folded);
}

// ---------------------------------------------------------------------------
// The profile

// Monotone, and strictly so where there is room: moving a node to a thriftier
// profile must never make its idle clock brighter. Checked across the whole
// domain, because the interesting failures are at the bottom of it where the
// floor starts flattening the steps together.
static void test_a_thriftier_profile_is_never_brighter() {
  for (uint8_t pct = 0; pct <= 100; pct++) {
    const uint8_t perf = rung(pct, Stage::Idle, kPerformance);
    const uint8_t bal  = rung(pct, Stage::Idle, kBalanced);
    const uint8_t batt = rung(pct, Stage::Idle, kBattery);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(perf, bal);
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(bal, batt);
  }
}

// The profile moves the idle rung and nothing else. Active is a promise and
// blank is off; only the stage nobody is reading is the profile's to trade.
static void test_the_profile_moves_only_the_idle_rung() {
  for (uint8_t pct = 0; pct <= 100; pct++)
    for (uint8_t p : kProfiles) {
      TEST_ASSERT_EQUAL_UINT8(rung(pct, Stage::Active, kPerformance),
                              rung(pct, Stage::Active, p));
      TEST_ASSERT_EQUAL_UINT8(0, rung(pct, Stage::Blank, p));
    }
}

// A profile ordinal this header does not know is answered as Performance —
// the brightest rung — rather than by reading past the end of the table. The
// enum has three values today; the day it grows a fourth this is what the
// backlight does until the table grows with it.
static void test_an_unknown_profile_falls_back_to_the_brightest_rung() {
  for (uint8_t p = BacklightLadder::kProfileCount; p < 16; p++)
    TEST_ASSERT_EQUAL_UINT8(rung(kDefaultPct, Stage::Idle, kPerformance),
                            rung(kDefaultPct, Stage::Idle, p));
  TEST_ASSERT_EQUAL_UINT8(rung(kDefaultPct, Stage::Idle, kPerformance),
                          rung(kDefaultPct, Stage::Idle, 255));
}

// ---------------------------------------------------------------------------
// Both ends

// Blank is off, everywhere. The panel stops the backlight itself when it goes
// dark; this is the same answer said by the rule, so the one place that
// writes a brightness has something to write on that edge rather than an
// exception to carry.
static void test_blank_is_off() {
  for (uint8_t pct = 0; pct <= 100; pct++)
    for (uint8_t p : kProfiles)
      TEST_ASSERT_EQUAL_UINT8(0, rung(pct, Stage::Blank, p));
}

// The idle clock stays a clock. Below the funnel's floor a backlight is not
// dim, it is off — and going dark is the blank stage's job, which is the same
// sentence the settings funnel says about a brightness of zero.
static void test_the_idle_clock_never_dims_below_the_floor() {
  for (uint8_t pct = kFloorPct; pct <= 100; pct++)
    for (uint8_t p : kProfiles)
      TEST_ASSERT_GREATER_OR_EQUAL_UINT8(kFloorPct, rung(pct, Stage::Idle, p));
}

// An operator already at the minimum has nothing left to give: the floor and
// the ceiling meet, and the idle clock is simply the working screen. The
// alternative — the floor winning — would light the idle screen brighter than
// the one being read.
static void test_at_the_minimum_setting_there_is_no_dim_step_left() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_EQUAL_UINT8(kFloorPct, rung(kFloorPct, Stage::Idle, p));
    TEST_ASSERT_EQUAL_UINT8(2, rung(2, Stage::Idle, p));    // below the funnel's floor
    TEST_ASSERT_EQUAL_UINT8(1, rung(1, Stage::Idle, p));
  }
}

// A brightness of zero is a panel the operator asked to be dark. The funnel
// refuses to store one, but nothing here may invent light out of it.
static void test_zero_stays_zero() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_EQUAL_UINT8(0, rung(0, Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(0, rung(0, Stage::Idle, p));
    TEST_ASSERT_EQUAL_UINT8(0, rung(0, Stage::Blank, p));
  }
}

// And past the top the answer is full brightness, not a wrapped surprise —
// the same clamp DisplayLayout::brightnessLevel makes, made here as well
// because the ladder's other consumer, the T-Deck's pulse dimmer, does not
// go through that mapping at all.
static void test_a_percentage_past_the_top_is_clamped() {
  for (uint8_t p : kProfiles) {
    TEST_ASSERT_EQUAL_UINT8(100, rung(101, Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(100, rung(255, Stage::Active, p));
    TEST_ASSERT_EQUAL_UINT8(rung(100, Stage::Idle, p), rung(255, Stage::Idle, p));
  }
}

// ---------------------------------------------------------------------------
// The shape of it

// Pure, and constant-folded: the whole ladder is answerable at compile time,
// which is what lets the display pass ask it once per pass without a thought.
static void test_the_rule_is_a_constant_expression() {
  static_assert(BacklightLadder::level(80, Stage::Active, kBattery) == 80, "");
  static_assert(BacklightLadder::level(80, Stage::Idle, kBattery) == 10, "");
  static_assert(BacklightLadder::level(80, Stage::Blank, kBattery) == 0, "");
  // The static_asserts above are the test; this keeps the case from passing
  // vacuously if somebody ever deletes them.
  constexpr uint8_t folded = BacklightLadder::level(80, Stage::Idle, kBattery);
  TEST_ASSERT_EQUAL_UINT8(10, folded);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_working_screen_is_the_configured_percentage);
  RUN_TEST(test_no_stage_ever_exceeds_the_setting);
  RUN_TEST(test_the_idle_clock_is_dimmer_than_the_working_screen);
  RUN_TEST(test_the_idle_rungs_are_the_numbers_they_are);
  RUN_TEST(test_the_stage_is_read_off_the_three_flags);
  RUN_TEST(test_the_stage_is_a_constant_expression);
  RUN_TEST(test_a_thriftier_profile_is_never_brighter);
  RUN_TEST(test_the_profile_moves_only_the_idle_rung);
  RUN_TEST(test_an_unknown_profile_falls_back_to_the_brightest_rung);
  RUN_TEST(test_blank_is_off);
  RUN_TEST(test_the_idle_clock_never_dims_below_the_floor);
  RUN_TEST(test_at_the_minimum_setting_there_is_no_dim_step_left);
  RUN_TEST(test_zero_stays_zero);
  RUN_TEST(test_a_percentage_past_the_top_is_clamped);
  RUN_TEST(test_the_rule_is_a_constant_expression);
  return UNITY_END();
}
