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


// PathWait: whether a message waits for a route, asks for one, or gives up.
//
// None of these states can be reached from a host test through the transport —
// they need a real path table, a real neighbour and a real 15 seconds. Pulled
// out here they are four comparisons, and the two that matter most are the ones
// nobody would exercise by hand: a message in flight across the millis() wrap,
// and a message the manners floor refuses to let ask.

#include <unity.h>
#include "PathWait.h"

using namespace Rns;

static PathWait waiting(uint32_t startedMs, bool asked = false, uint32_t askedMs = 0) {
  PathWait w;
  w.startedMs = startedMs;
  w.asked     = asked;
  w.askedMs   = askedMs;
  return w;
}

// --- a path already exists ---------------------------------------------------

static void test_a_known_path_sends_at_once() {
  TEST_ASSERT_EQUAL((int)PathAction::Send,
                    (int)pathAction(true, waiting(0), 0, kNeverRequested));
}

static void test_a_path_that_arrives_while_waiting_sends() {
  // The whole point of waiting. Asked at 1000, path turns up at 4000.
  const PathWait w = waiting(1000, true, 1000);
  TEST_ASSERT_EQUAL((int)PathAction::Wait,
                    (int)pathAction(false, w, 4000, 0));
  TEST_ASSERT_EQUAL((int)PathAction::Send,
                    (int)pathAction(true, w, 4000, 0));
}

static void test_a_path_arriving_after_the_window_is_still_used() {
  // Deliberate: the message has not been thrown away yet, and a route that
  // exists beats a deadline that has just passed. Refusing here would spend the
  // message for the sake of tidiness.
  const PathWait w = waiting(1000, true, 1000);
  TEST_ASSERT_EQUAL((int)PathAction::GiveUp,
                    (int)pathAction(false, w, 1000 + kPathWaitMs, 0));
  TEST_ASSERT_EQUAL((int)PathAction::Send,
                    (int)pathAction(true, w, 1000 + kPathWaitMs, 0));
}

// --- asking ------------------------------------------------------------------

static void test_a_first_message_to_an_unknown_destination_asks() {
  TEST_ASSERT_EQUAL((int)PathAction::Request,
                    (int)pathAction(false, waiting(500), 500, kNeverRequested));
}

static void test_it_asks_only_once_per_message() {
  // Having asked, every later pass waits — a request per pass would be a
  // broadcast per pass.
  const PathWait w = waiting(500, true, 500);
  for (uint32_t t = 600; t < 500 + kPathWaitMs; t += 1000)
    TEST_ASSERT_EQUAL((int)PathAction::Wait, (int)pathAction(false, w, t, 0));
}

static void test_the_manners_floor_refuses_a_second_ask_too_soon() {
  // A second message to the same dead destination, 5 s after the first asked.
  // It waits out its own window without asking again.
  TEST_ASSERT_EQUAL((int)PathAction::Wait,
                    (int)pathAction(false, waiting(20000), 20000, 5000));
}

static void test_the_floor_lifts_once_the_interval_has_passed() {
  TEST_ASSERT_EQUAL((int)PathAction::Request,
                    (int)pathAction(false, waiting(30000), 30000,
                                    kPathRequestMinIntervalMs));
}

static void test_the_floor_is_longer_than_the_wait() {
  // Not an accident: one message gets one request, and the next message to the
  // same destination cannot re-ask before its own window would have closed.
  TEST_ASSERT_TRUE(kPathRequestMinIntervalMs > kPathWaitMs);
}

static void test_never_requested_is_not_zero() {
  // millis() is 0 exactly once per boot. If "never asked" were spelled 0, the
  // first send of every run would be read as "asked just now" and refused by
  // the manners floor.
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, kNeverRequested);
  TEST_ASSERT_EQUAL((int)PathAction::Request,
                    (int)pathAction(false, waiting(0), 0, kNeverRequested));
}

// --- giving up ---------------------------------------------------------------

static void test_the_wait_is_bounded() {
  const PathWait w = waiting(1000, true, 1000);
  TEST_ASSERT_EQUAL((int)PathAction::Wait,
                    (int)pathAction(false, w, 1000 + kPathWaitMs - 1, 0));
  TEST_ASSERT_EQUAL((int)PathAction::GiveUp,
                    (int)pathAction(false, w, 1000 + kPathWaitMs, 0));
}

static void test_a_message_the_floor_never_let_ask_still_gives_up() {
  // The bug this shape exists to avoid. Bound the wait from the request and a
  // message that was never allowed to request has no deadline running: it waits
  // for ever, holding the queue, on a node that looks idle.
  const PathWait never = waiting(1000);          // asked == false, all along
  TEST_ASSERT_EQUAL((int)PathAction::Wait,
                    (int)pathAction(false, never, 5000, 0));
  TEST_ASSERT_EQUAL((int)PathAction::GiveUp,
                    (int)pathAction(false, never, 1000 + kPathWaitMs, 0));
}

// --- the clock ---------------------------------------------------------------

static void test_a_wait_across_the_millis_wrap_is_still_short() {
  // millis() wraps about every 49 days. Started 1 s before the wrap, still
  // inside the window 2 s after it.
  const uint32_t started = 0xFFFFFFFFu - 1000;
  const PathWait w = waiting(started, true, started);
  TEST_ASSERT_EQUAL((int)PathAction::Wait, (int)pathAction(false, w, 2000, 0));
}

static void test_the_bound_still_fires_across_the_wrap() {
  const uint32_t started = 0xFFFFFFFFu - 1000;
  const PathWait w = waiting(started, true, started);
  TEST_ASSERT_EQUAL((int)PathAction::GiveUp,
                    (int)pathAction(false, w, started + kPathWaitMs, 0));
}

static void test_the_manners_floor_survives_the_wrap() {
  // sinceLastRequestMs is computed by the caller as a wrapped difference; it
  // arrives here already correct, so a huge value must read as "long ago"
  // rather than as an overflow to be distrusted.
  TEST_ASSERT_EQUAL((int)PathAction::Request,
                    (int)pathAction(false, waiting(10), 10, 0xFFFFFFFEu));
}

// --- the reason a message failed ---------------------------------------------

static void test_every_failure_has_a_name_and_none_is_empty() {
  TEST_ASSERT_EQUAL_STRING("no path", failureName(SendFailure::NoPath));
  TEST_ASSERT_EQUAL_STRING("no key",  failureName(SendFailure::NoKey));
  TEST_ASSERT_EQUAL_STRING("refused", failureName(SendFailure::Refused));
}

static void test_no_failure_names_nothing() {
  // The surfaces print this only when there is something to say; a word here
  // would put "none" beside every delivered message.
  TEST_ASSERT_EQUAL_STRING("", failureName(SendFailure::None));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_known_path_sends_at_once);
  RUN_TEST(test_a_path_that_arrives_while_waiting_sends);
  RUN_TEST(test_a_path_arriving_after_the_window_is_still_used);
  RUN_TEST(test_a_first_message_to_an_unknown_destination_asks);
  RUN_TEST(test_it_asks_only_once_per_message);
  RUN_TEST(test_the_manners_floor_refuses_a_second_ask_too_soon);
  RUN_TEST(test_the_floor_lifts_once_the_interval_has_passed);
  RUN_TEST(test_the_floor_is_longer_than_the_wait);
  RUN_TEST(test_never_requested_is_not_zero);
  RUN_TEST(test_the_wait_is_bounded);
  RUN_TEST(test_a_message_the_floor_never_let_ask_still_gives_up);
  RUN_TEST(test_a_wait_across_the_millis_wrap_is_still_short);
  RUN_TEST(test_the_bound_still_fires_across_the_wrap);
  RUN_TEST(test_the_manners_floor_survives_the_wrap);
  RUN_TEST(test_every_failure_has_a_name_and_none_is_empty);
  RUN_TEST(test_no_failure_names_nothing);
  return UNITY_END();
}
