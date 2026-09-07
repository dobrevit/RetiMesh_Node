// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// How long the radio task is allowed to park, and why each answer is the one
// it is. Runs on the host: pio test -e native
//
// The rule guards two failures that nothing else in the project would catch.
//
//   * Dropping the duty-lock case turns the park into a busy loop. While the
//     hourly transmit budget is spent the TX ring is not drained, so a park
//     that skipped itself because something is in it would run this task at
//     priority 5 until the window slides — an hour of full-tilt core on a
//     solar node. Nothing instrumented would report it: tools/soak.py samples
//     heap, stacks and counters, none of which move, and the node keeps
//     relaying throughout.
//   * Dropping the flags case costs a restart its budget. Bootloader.cpp gives
//     the radio 250 ms to sleep the chip, and a request that had to wait out a
//     full park would spend 100 ms of it doing nothing at all.
//
// Both are decisions about numbers, made from inputs the caller already has,
// with no kernel interleaving in them — so they are pinned here rather than
// inferred from a bench session with an ammeter.
#include <unity.h>
#include <stdint.h>
#include "RadioParkPolicy.h"

// The firmware's own figure, so a change to kIdleWaitMs does not quietly
// change what these tests mean. Any positive value would do; this is the one
// the node runs.
static const uint32_t kIdle = 100;

// The anti-spin case, and the reason this file exists. Packets are waiting and
// the transmit budget says none of them is going anywhere, so the task parks
// for the full interval instead of coming straight back to the same answer.
static void test_a_duty_locked_pass_parks_even_with_packets_waiting() {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kIdle, RadioPark::waitMs(true, 1, false, kIdle),
                                   "nothing will drain the ring under a duty lock: do not spin");
  // However many are waiting. A deep backlog is the case where the spin would
  // last longest, not a reason to skip the park.
  TEST_ASSERT_EQUAL_UINT32(kIdle, RadioPark::waitMs(true, 64, false, kIdle));
}

// Unlocked, with something in the ring: the next pass takes an item and
// transmits it, so back-to-back packets are not paced by the idle timer.
static void test_a_queued_packet_is_not_made_to_wait() {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, RadioPark::waitMs(false, 1, false, kIdle),
                                   "a packet in the ring is work for the very next pass");
  TEST_ASSERT_EQUAL_UINT32(0, RadioPark::waitMs(false, 64, false, kIdle));
}

// The ordinary idle node: nothing queued, nothing asked for, nothing to do but
// keep the beacon clock and the watchdog. This is the wait the whole round was
// widened for, and shortening it back is the regression on the other side.
static void test_an_idle_pass_waits_the_whole_interval() {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kIdle, RadioPark::waitMs(false, 0, false, kIdle),
                                   "an idle task parks; the timer is for what nobody signals");
}

// A sleep or a reconfigure beats every other input, duty lock included. The
// producer sets its flag and calls wake(); the task publishes _parked and then
// reads the flag, and this is the half of that pairing that makes the request
// unmissable — a flag seen here is served on the next pass rather than 100 ms
// later, out of a restart budget of 250.
//
// Duty-locked plus flags is the case worth being explicit about: it returns
// zero, unlike duty-locked plus a queued packet. The difference is not the
// urgency, it is whether the skip repeats. A flag is consumed unconditionally
// at the top of the pass this returns to, so the park is skipped exactly once;
// a queued packet under a duty lock is still queued next pass, and every pass
// after that, which is the spin above.
static void test_a_pending_request_is_served_whatever_else_is_true() {
  for (int locked = 0; locked <= 1; locked++) {
    for (size_t queued = 0; queued <= 2; queued++) {
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, RadioPark::waitMs(locked != 0, queued, true, kIdle),
                                       "a restart or a settings apply waits for nothing");
    }
  }
}

// The interval is the caller's, not the rule's: the park is sized against the
// watchdog and the beacon clock in LoRaRadio.cpp, and this function only says
// whether to wait it out.
static void test_the_interval_comes_from_the_caller() {
  TEST_ASSERT_EQUAL_UINT32(10, RadioPark::waitMs(false, 0, false, 10));
  TEST_ASSERT_EQUAL_UINT32(5000, RadioPark::waitMs(true, 3, false, 5000));
  // A caller that asks for no interval at all still gets a well-formed answer
  // rather than a park of some other length.
  TEST_ASSERT_EQUAL_UINT32(0, RadioPark::waitMs(false, 0, false, 0));
}

// Whatever the inputs, the answer is one of two values. A park is never longer
// than the interval the watchdog feed is sized against, and never some third
// number arrived at by arithmetic.
static void test_the_answer_is_always_one_of_the_two_waits() {
  for (int locked = 0; locked <= 1; locked++) {
    for (int flags = 0; flags <= 1; flags++) {
      for (size_t queued = 0; queued <= 3; queued++) {
        const uint32_t ms = RadioPark::waitMs(locked != 0, queued, flags != 0, kIdle);
        TEST_ASSERT_TRUE_MESSAGE(ms == 0 || ms == kIdle,
                                 "the park either skips or waits the interval, never between");
      }
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_duty_locked_pass_parks_even_with_packets_waiting);
  RUN_TEST(test_a_queued_packet_is_not_made_to_wait);
  RUN_TEST(test_an_idle_pass_waits_the_whole_interval);
  RUN_TEST(test_a_pending_request_is_served_whatever_else_is_true);
  RUN_TEST(test_the_interval_comes_from_the_caller);
  RUN_TEST(test_the_answer_is_always_one_of_the_two_waits);
  return UNITY_END();
}
