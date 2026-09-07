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


// DisplayPace: how long the display task may sleep between passes, given
// what the GUI shell asked for. Worth pinning here rather than judging on a
// bench, because both ways of getting it wrong are hard to see and easy to
// ship: obey the shell and a screen with no pending timer sleeps for seven
// weeks with the button unread, or bound it too tightly and the whole saving
// quietly becomes the fixed pass it replaced. Neither is a compile error and
// neither shows up in a screenshot.
//
// What the shell actually returns is not faked here and is not this file's
// business: it is LVGL's own arithmetic over its timer list. What is pinned
// is every answer this rule gives for every figure that arithmetic can
// produce, including the two edges it produces most often — zero when a
// timer is already due, and 0xFFFFFFFF when none is pending.
#include <unity.h>
#include <stdint.h>
#include "../../src/ui/DisplayPace.h"

// LVGL's "no timer is pending" answer (LV_NO_TIMER_READY), written out rather
// than included: this test is not an LVGL build, and the one figure that
// would break this rule hardest deserves to be read twice in a diff.
static constexpr uint32_t kNoTimerReady = 0xFFFFFFFFu;

// LVGL's default refresh period (LV_DEF_REFR_PERIOD), which the shell does not
// override. Its display refresh timer and each of its input read timers run at
// it, so it is what the shell asks for while it sits on a lit screen with
// nothing happening — the case this whole rule exists to make cheaper.
static constexpr uint32_t kShellIdleRequestMs = 33;

// The blanked panel's wake poll (Display.cpp) and the bench criterion the
// backlight work measured wake-from-blank against. Written out for the same
// reason: this rule is chosen partly to protect that budget, and the budget
// has to be visible here for the choice to be checkable.
static constexpr uint32_t kWakePollMs   = 250;
static constexpr uint32_t kWakeBudgetMs = 300;

// ---------------------------------------------------------------------------
// The floor

// A shell mid-animation asks to be called back at once. The task has other
// duties and a tick to respect, so the shortest pass it may run is the pass
// it has always run.
static void test_a_request_below_the_floor_gets_the_floor() {
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMinMs, DisplayPace::passMs(0));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMinMs, DisplayPace::passMs(1));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMinMs, DisplayPace::passMs(DisplayPace::kMinMs - 1));
}

// The floor is not a new number. It is BUTTON_POLL_MS — the cadence every
// other duty on this task was written against — so a busy screen runs exactly
// the loop it ran before this rule existed.
static void test_the_floor_is_the_pass_the_task_always_ran() {
  TEST_ASSERT_EQUAL_UINT32((uint32_t)BUTTON_POLL_MS, DisplayPace::kMinMs);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)BUTTON_POLL_MS, DisplayPace::passMs(0));
}

// ---------------------------------------------------------------------------
// The ceiling

// The one that matters: LVGL answers 0xFFFFFFFF when its timer list holds
// nothing pending. Obeyed, that is a display task asleep for seven weeks with
// a button nobody is reading.
static void test_no_timer_pending_does_not_sleep_for_seven_weeks() {
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(kNoTimerReady));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(UINT32_MAX));
}

static void test_a_request_above_the_ceiling_gets_the_ceiling() {
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(DisplayPace::kMaxMs + 1));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(1000));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(60000));
}

// Why the ceiling is the number it is: a tap costs three passes in the worst
// case — one to sample a finger that is down, two to prove it has lifted —
// and those three have to fit inside the tenth of a second a person reads as
// an immediate response.
static void test_three_passes_fit_inside_the_response_budget() {
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(DisplayPace::kResponseMs,
                                   DisplayPace::kMaxMs * DisplayPace::kPassesPerTap);
  // And the pass the task ran before this rule is comfortably inside it, so
  // the ceiling spends headroom rather than borrowing against it.
  TEST_ASSERT_LESS_THAN_UINT32(DisplayPace::kResponseMs,
                               DisplayPace::kMinMs * DisplayPace::kPassesPerTap);
}

// The task feeds the watchdog once per pass, so the ceiling is also the
// longest a healthy pass goes without reporting — before the pass's own work
// is counted at all.
static void test_the_ceiling_sits_well_inside_the_watchdog() {
  TEST_ASSERT_LESS_THAN_UINT32((uint32_t)WATCHDOG_TIMEOUT_S * 1000UL, DisplayPace::kMaxMs);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32((uint32_t)WATCHDOG_TIMEOUT_S * 1000UL,
                                   DisplayPace::kMaxMs * 100);
}

// ---------------------------------------------------------------------------
// The middle, which is the whole point

// What the shell actually asks for on an idle lit screen passes through
// untouched. If this ever starts failing at the ceiling, the saving has been
// silently halved and the only symptom is a battery.
static void test_the_shells_idle_request_passes_through() {
  TEST_ASSERT_EQUAL_UINT32(kShellIdleRequestMs, DisplayPace::passMs(kShellIdleRequestMs));
  TEST_ASSERT_EQUAL_UINT32(25u, DisplayPace::passMs(25));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMinMs, DisplayPace::passMs(DisplayPace::kMinMs));
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, DisplayPace::passMs(DisplayPace::kMaxMs));
}

// And it is a real saving, not a rounding of one: an idle lit screen sleeps
// longer than the fixed pass it replaced.
static void test_an_idle_screen_sleeps_longer_than_it_used_to() {
  TEST_ASSERT_GREATER_THAN_UINT32((uint32_t)BUTTON_POLL_MS,
                                  DisplayPace::passMs(kShellIdleRequestMs));
}

// ---------------------------------------------------------------------------
// The shape of it

// Every answer is a pass the task can actually run, whatever it was handed.
static void test_every_answer_is_inside_the_bounds() {
  const uint32_t requests[] = { 0, 1, 2, 5, 19, 20, 21, 32, 33, 34, 50, 99, 100,
                                250, 1000, 65535, 1000000, kNoTimerReady };
  for (uint32_t r : requests) {
    const uint32_t p = DisplayPace::passMs(r);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(DisplayPace::kMinMs, p);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(DisplayPace::kMaxMs, p);
  }
}

// Asking for longer never gets a shorter pass. A clamp written the other way
// round would still satisfy the two ends and fail here.
static void test_a_longer_request_never_shortens_the_pass() {
  uint32_t last = DisplayPace::passMs(0);
  for (uint32_t r = 1; r <= 200; r++) {
    const uint32_t p = DisplayPace::passMs(r);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(last, p);
    last = p;
  }
}

// ---------------------------------------------------------------------------
// What it must not disturb

// Wake-from-blank was measured against a 300 ms criterion assuming a floor
// length pass, and it keeps exactly that: a blanked panel is not paced by
// this rule, so its worst case is the wake poll plus one floor pass. The
// second assertion is the ceiling earning its number a second time — even if
// a blanked panel were ever paced by the shell's request, the budget still
// holds, with the panel's own wake work inside the remainder.
static void test_the_wake_from_blank_budget_survives_this() {
  TEST_ASSERT_LESS_THAN_UINT32(kWakeBudgetMs, kWakePollMs + DisplayPace::kMinMs);
  TEST_ASSERT_LESS_THAN_UINT32(kWakeBudgetMs, kWakePollMs + DisplayPace::kMaxMs);
}

// Pure and constant-folded, like the rest of this round's rules: the pass can
// ask it without a thought, and a retune that broke the folding would be a
// build failure here rather than a surprise in a profile.
static void test_the_rule_is_a_constant_expression() {
  static_assert(DisplayPace::passMs(0) == DisplayPace::kMinMs, "");
  static_assert(DisplayPace::passMs(33) == 33, "");
  static_assert(DisplayPace::passMs(0xFFFFFFFFu) == DisplayPace::kMaxMs, "");
  constexpr uint32_t folded = DisplayPace::passMs(0xFFFFFFFFu);
  TEST_ASSERT_EQUAL_UINT32(DisplayPace::kMaxMs, folded);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_request_below_the_floor_gets_the_floor);
  RUN_TEST(test_the_floor_is_the_pass_the_task_always_ran);
  RUN_TEST(test_no_timer_pending_does_not_sleep_for_seven_weeks);
  RUN_TEST(test_a_request_above_the_ceiling_gets_the_ceiling);
  RUN_TEST(test_three_passes_fit_inside_the_response_budget);
  RUN_TEST(test_the_ceiling_sits_well_inside_the_watchdog);
  RUN_TEST(test_the_shells_idle_request_passes_through);
  RUN_TEST(test_an_idle_screen_sleeps_longer_than_it_used_to);
  RUN_TEST(test_every_answer_is_inside_the_bounds);
  RUN_TEST(test_a_longer_request_never_shortens_the_pass);
  RUN_TEST(test_the_wake_from_blank_budget_survives_this);
  RUN_TEST(test_the_rule_is_a_constant_expression);
  return UNITY_END();
}
