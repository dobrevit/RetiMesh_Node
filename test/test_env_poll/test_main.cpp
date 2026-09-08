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


// EnvPollPolicy: which of two passes of the loop a BME280 reading is. Worth
// pinning because every way of getting it wrong is silent — a converting flag
// left set is a sensor reporting the same number for ever, a collect that runs
// twice reads a conversion already consumed, and a failed trigger treated as
// started reads a register the part never filled. None of those is a crash and
// none is visible on a bench without instrumenting the driver.
//
// What the I2C conversation itself does is not tested here and is not faked:
// triggering a conversion is a register write to a part that has to answer,
// and the only place that is proved is a bench.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/EnvPollPolicy.h"

using Action = EnvPollPolicy::Action;

static constexpr uint32_t kConvertMs = 20;
static constexpr bool kDue    = true;
static constexpr bool kNotDue = false;

// ---------------------------------------------------------------------------
// The ordinary cycle

// Nothing due and nothing in flight is the answer on almost every pass, and it
// has to cost nothing: this runs hundreds of times a second.
static void test_nothing_due_is_idle() {
  const EnvPollPolicy p(kConvertMs);
  TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(0, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(100000, kNotDue));
  TEST_ASSERT_FALSE(p.converting());
}

static void test_due_triggers() {
  const EnvPollPolicy p(kConvertMs);
  TEST_ASSERT_EQUAL((int)Action::Trigger, (int)p.decide(1000, kDue));
}

// The window, from both sides of its edge.
static void test_a_conversion_waits_its_window_out_then_collects() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, true);
  TEST_ASSERT_TRUE(p.converting());
  TEST_ASSERT_EQUAL((int)Action::Wait,    (int)p.decide(1000, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Wait,    (int)p.decide(1019, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(1020, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(9999, kNotDue));
}

// And the gate is not consulted while a conversion is in flight: the answer it
// produced would be thrown away, and a reading already paid for is worth
// collecting whatever the cadence now thinks.
static void test_the_cadence_does_not_interrupt_a_conversion_in_flight() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, true);
  TEST_ASSERT_EQUAL((int)Action::Wait,    (int)p.decide(1010, kDue));
  TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(1030, kDue));
}

// Collect is what the caller acts on, and finished() is what ends the flight.
// Deciding must not end it by itself, or a read that failed would be recorded
// as a reading taken.
static void test_deciding_to_collect_does_not_end_the_flight() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, true);
  (void)p.decide(1030, kNotDue);
  TEST_ASSERT_TRUE(p.converting());
  (void)p.decide(1040, kNotDue);
  TEST_ASSERT_TRUE(p.converting());
  p.finished();
  TEST_ASSERT_FALSE(p.converting());
}

// After a collection the next reading waits for the cadence, and is not
// triggered again on the very next pass.
static void test_after_collecting_the_next_reading_waits_for_the_cadence() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, true);
  (void)p.decide(1030, kNotDue);
  p.finished();
  TEST_ASSERT_EQUAL((int)Action::Idle,    (int)p.decide(1031, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Trigger, (int)p.decide(31000, kDue));
}

// ---------------------------------------------------------------------------
// The part that refuses

// The one the driver had no way to prove: a trigger the part did not take must
// leave nothing in flight, so the next pass cannot decide to collect a
// conversion that was never started — which would read a register the part
// never filled and publish it as a measurement.
static void test_a_refused_trigger_starts_no_conversion() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, false);
  TEST_ASSERT_FALSE(p.converting());
  TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(1030, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(9999, kNotDue));
}

// And it does not retry on the next pass either: the cadence has spent its
// turn, so a silent part is asked again at the next interval rather than
// hundreds of times a second on a bus it shares with the panel.
static void test_a_refused_trigger_does_not_retry_until_the_cadence_says_so() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, false);
  // the caller's gate answers not-due until its interval is up, and the policy
  // must not manufacture a trigger of its own in the meantime
  for (uint32_t t = 1001; t < 1200; t++)
    TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(t, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Trigger, (int)p.decide(31000, kDue));
}

// A part that is still busy when the window closes is abandoned rather than
// read: finished() ends the flight and the reading waits its turn.
static void test_an_abandoned_conversion_returns_to_idle() {
  EnvPollPolicy p(kConvertMs);
  p.triggered(1000, true);
  TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(1030, kNotDue));
  p.finished();                                  // still busy: abandoned
  TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(1031, kNotDue));
}

// ---------------------------------------------------------------------------
// The clock

// millis() wraps every forty-nine days and a node is expected to outlive that.
// A conversion started either side of the wrap has to close its window on
// elapsed time rather than on a comparison that goes backwards.
static void test_the_window_holds_across_a_millis_wrap() {
  EnvPollPolicy p(kConvertMs);
  const uint32_t justBefore = 0xFFFFFFF0u;       // 16 ms before the wrap
  p.triggered(justBefore, true);
  TEST_ASSERT_EQUAL((int)Action::Wait,    (int)p.decide(justBefore + 5, kNotDue));
  TEST_ASSERT_EQUAL((int)Action::Wait,    (int)p.decide((uint32_t)(justBefore + 19), kNotDue));
  // 0xFFFFFFF0 + 20 wraps to 4
  TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(4, kNotDue));
}

// A thousand cycles, because the flags are the thing that would decay: a
// policy that answered on a count rather than on its state would drift out of
// step somewhere in here, and a converting flag that was never cleared would
// stop producing Trigger at all.
static void test_a_thousand_cycles_stay_in_step() {
  EnvPollPolicy p(kConvertMs);
  uint32_t now = 0;
  int collected = 0;
  for (int i = 0; i < 1000; i++) {
    TEST_ASSERT_EQUAL((int)Action::Trigger, (int)p.decide(now, kDue));
    p.triggered(now, true);
    now += 10;
    TEST_ASSERT_EQUAL((int)Action::Wait, (int)p.decide(now, kNotDue));
    now += 15;
    TEST_ASSERT_EQUAL((int)Action::Collect, (int)p.decide(now, kNotDue));
    p.finished();
    collected++;
    now += 30000;
    TEST_ASSERT_EQUAL((int)Action::Idle, (int)p.decide(now, kNotDue));
  }
  TEST_ASSERT_EQUAL_INT(1000, collected);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_due_is_idle);
  RUN_TEST(test_due_triggers);
  RUN_TEST(test_a_conversion_waits_its_window_out_then_collects);
  RUN_TEST(test_the_cadence_does_not_interrupt_a_conversion_in_flight);
  RUN_TEST(test_deciding_to_collect_does_not_end_the_flight);
  RUN_TEST(test_after_collecting_the_next_reading_waits_for_the_cadence);
  RUN_TEST(test_a_refused_trigger_starts_no_conversion);
  RUN_TEST(test_a_refused_trigger_does_not_retry_until_the_cadence_says_so);
  RUN_TEST(test_an_abandoned_conversion_returns_to_idle);
  RUN_TEST(test_the_window_holds_across_a_millis_wrap);
  RUN_TEST(test_a_thousand_cycles_stay_in_step);
  return UNITY_END();
}
