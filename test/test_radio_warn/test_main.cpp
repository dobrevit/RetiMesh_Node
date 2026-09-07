// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// How often a repeating radio failure may reach the log. Runs on the host:
// pio test -e native
//
// The rule paces two faults that repeat at packet rate — a transceiver that
// fails every CAD probe, and a driver that refuses the duty-cycled receive on
// every re-arm — and it has to get three things right at once, none of which
// is visible from the outside until it is already wrong:
//
//   * the first failure of a kind is never suppressed. Get that backwards and
//     a node that has just gone deaf says nothing for a whole minute, which is
//     exactly the minute an operator is looking at the log.
//   * repeats inside the interval are. Get that backwards and one deferral is
//     fifty blocking console writes, emitted from the task that is trying to
//     transmit.
//   * the two kinds cannot suppress one another. They share a rule, not a
//     stamp; a CAD storm that silenced the arm warning would hide the fault
//     the rx_duty_cycle feature exists to report.
//
// It used to read millis() for itself, so none of this could be pinned without
// a bench session and a wedged part. The clock is an argument now.
#include <unity.h>
#include <stdint.h>
#include "RadioWarnPolicy.h"

// The firmware's own figure (LoRaRadio.cpp), so a change to it does not quietly
// change what these tests mean.
static const uint32_t kInterval = 60000;

// The case an operator sees: something has just broken, and the log says so
// now rather than at the top of the next minute. count arrives as 1 because
// the caller increments before it asks.
static void test_the_first_failure_of_a_kind_always_logs() {
  uint32_t stamp = 0;
  TEST_ASSERT_TRUE_MESSAGE(RadioWarn::due(stamp, 1, 5000, kInterval),
                           "a fault must be reported the moment it starts");
  TEST_ASSERT_EQUAL_UINT32(5000, stamp);

  // Even when the stamp is fresh — a first failure is not a repeat of anything,
  // whatever the clock says. This is the ordering that matters: the count test
  // comes first, so a stamp left behind by a different run cannot swallow it.
  uint32_t warm = 5000;
  TEST_ASSERT_TRUE(RadioWarn::due(warm, 1, 5001, kInterval));
  TEST_ASSERT_EQUAL_UINT32(5001, warm);
}

// ...and the case the rule exists for: the same fault, again, immediately.
static void test_a_repeat_inside_the_interval_is_suppressed() {
  uint32_t stamp = 0;
  TEST_ASSERT_TRUE(RadioWarn::due(stamp, 1, 1000, kInterval));   // the first one
  TEST_ASSERT_FALSE(RadioWarn::due(stamp, 2, 1001, kInterval));
  TEST_ASSERT_FALSE(RadioWarn::due(stamp, 50, 30000, kInterval));
  // A suppressed warning must not move the stamp, or a storm at just under the
  // interval would push the next line out for ever.
  TEST_ASSERT_EQUAL_UINT32(1000, stamp);

  // One tick short of the interval is still inside it; the interval itself is
  // not. Written as the boundary rather than as a round number because "<" and
  // "<=" are the same length and only one of them is the rule.
  TEST_ASSERT_FALSE(RadioWarn::due(stamp, 99, 1000 + kInterval - 1, kInterval));
  TEST_ASSERT_TRUE(RadioWarn::due(stamp, 99, 1000 + kInterval, kInterval));
  TEST_ASSERT_EQUAL_UINT32(1000 + kInterval, stamp);
}

// The whole reason the stamp is a reference the caller owns rather than state
// this rule keeps for itself.
static void test_two_kinds_of_failure_do_not_suppress_each_other() {
  uint32_t cad = 0, arm = 0;
  TEST_ASSERT_TRUE(RadioWarn::due(cad, 1, 1000, kInterval));
  // The CAD storm continues, and the driver refuses an arm in the middle of it.
  for (uint32_t n = 2; n < 60; n++)
    TEST_ASSERT_FALSE(RadioWarn::due(cad, n, 1000 + n, kInterval));
  TEST_ASSERT_TRUE_MESSAGE(RadioWarn::due(arm, 1, 1030, kInterval),
                           "a CAD storm must not silence the arm warning");
  // ...and in the other direction: the arm failure now repeating does not
  // release the CAD line early.
  TEST_ASSERT_FALSE(RadioWarn::due(arm, 2, 1031, kInterval));
  TEST_ASSERT_FALSE(RadioWarn::due(cad, 60, 1060, kInterval));
  TEST_ASSERT_EQUAL_UINT32(1000, cad);
  TEST_ASSERT_EQUAL_UINT32(1030, arm);
}

// A node that has been up for 49 days is not a node that stops warning. The
// subtraction is unsigned for the same reason every other millis() comparison
// in the firmware is.
static void test_the_rule_survives_the_millis_wrap() {
  const uint32_t nearTop = 0xFFFFFFFFUL - 1000;
  uint32_t stamp = nearTop;
  // 2000 ms later the counter has wrapped past zero. Well inside the interval,
  // so still suppressed — a wrap must not be read as "an interval has passed".
  TEST_ASSERT_FALSE(RadioWarn::due(stamp, 2, 1000, kInterval));
  TEST_ASSERT_EQUAL_UINT32(nearTop, stamp);
  // ...and once a genuine interval has elapsed across the wrap, it logs.
  TEST_ASSERT_TRUE(RadioWarn::due(stamp, 3, nearTop + kInterval, kInterval));
}

// The interval belongs to the caller, exactly as the park interval does. Both
// call sites in the radio pass the same one; nothing here assumes it.
static void test_the_interval_comes_from_the_caller() {
  uint32_t stamp = 0;
  TEST_ASSERT_TRUE(RadioWarn::due(stamp, 1, 100, 10));
  TEST_ASSERT_FALSE(RadioWarn::due(stamp, 2, 109, 10));
  TEST_ASSERT_TRUE(RadioWarn::due(stamp, 3, 110, 10));
  // Zero means every failure is logged, which is what a caller that wanted no
  // pacing at all would ask for; it must not divide, wrap or refuse.
  uint32_t open = 0;
  TEST_ASSERT_TRUE(RadioWarn::due(open, 1, 5, 0));
  TEST_ASSERT_TRUE(RadioWarn::due(open, 2, 5, 0));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_failure_of_a_kind_always_logs);
  RUN_TEST(test_a_repeat_inside_the_interval_is_suppressed);
  RUN_TEST(test_two_kinds_of_failure_do_not_suppress_each_other);
  RUN_TEST(test_the_rule_survives_the_millis_wrap);
  RUN_TEST(test_the_interval_comes_from_the_caller);
  return UNITY_END();
}
