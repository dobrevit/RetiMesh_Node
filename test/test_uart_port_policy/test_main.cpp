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
//  Which UART already belongs to something (issue 9).
//
//  This rule lived inside UartLink, where nothing could reach it: that file is
//  behind HAS_SERIAL_LINK, no board sets it, and the one env that does ignores
//  tests. The regression case below is the reason it was worth moving — it
//  fails against the version that was there, and passes against this one.
// ============================================================================
#include <unity.h>
#include "../../src/net/UartPortPolicy.h"

// A bridged board with a receiver: console on the bridge UART, GNSS on 1.
static constexpr UartPort::Owners kBridgedWithGps{0, true, 1};
// A native-USB board with no receiver — the console is still on a port.
static constexpr UartPort::Owners kNoGps{0, false, -1};

void setUp() {}
void tearDown() {}

static void test_the_console_port_is_never_free() {
  TEST_ASSERT_TRUE(UartPort::taken(0, kBridgedWithGps));
  TEST_ASSERT_TRUE(UartPort::taken(0, kNoGps));
}

static void test_the_receivers_port_is_taken_only_where_there_is_a_receiver() {
  TEST_ASSERT_TRUE(UartPort::taken(1, kBridgedWithGps));
  TEST_ASSERT_FALSE(UartPort::taken(1, kNoGps));
}

static void test_a_port_nobody_claimed_is_free() {
  TEST_ASSERT_FALSE(UartPort::taken(2, kBridgedWithGps));
  TEST_ASSERT_FALSE(UartPort::taken(2, kNoGps));
}

static void test_a_console_that_is_not_on_uart_zero_is_still_the_consoles() {
  // The regression, and the reason this rule was extracted. The version inside
  // the driver tested the macro when PPP was compiled in and a literal 0 when
  // it was not — which agrees on every board today, and Config.h says the
  // instance is 0 "on every board so far". On the first board with its console
  // elsewhere, the serial link would have been handed the console's own port.
  constexpr UartPort::Owners consoleOnTwo{2, false, -1};
  TEST_ASSERT_TRUE(UartPort::taken(2, consoleOnTwo));
  TEST_ASSERT_FALSE(UartPort::taken(0, consoleOnTwo));
}

static void test_whether_ppp_exists_changes_nothing() {
  // PPP is not a parameter on purpose: it shares the bridge UART with the
  // console through an arbiter rather than owning it instead of the console,
  // so the port is unavailable either way. Asserted so that nobody adds a
  // `hasPpp` flag back and makes the answer depend on it.
  constexpr UartPort::Owners a{0, true, 1};
  constexpr UartPort::Owners b{0, true, 1};
  TEST_ASSERT_EQUAL(UartPort::taken(0, a), UartPort::taken(0, b));
  TEST_ASSERT_TRUE(UartPort::taken(0, a));
}

static void test_a_negative_port_is_not_a_port() {
  TEST_ASSERT_TRUE(UartPort::taken(-1, kBridgedWithGps));
}

static void test_a_board_with_no_receiver_does_not_need_a_gps_port_number() {
  // gpsUart is -1 here and must never be compared against, or a candidate of
  // -1 would collide with it for the wrong reason.
  TEST_ASSERT_FALSE(UartPort::taken(3, kNoGps));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_console_port_is_never_free);
  RUN_TEST(test_the_receivers_port_is_taken_only_where_there_is_a_receiver);
  RUN_TEST(test_a_port_nobody_claimed_is_free);
  RUN_TEST(test_a_console_that_is_not_on_uart_zero_is_still_the_consoles);
  RUN_TEST(test_whether_ppp_exists_changes_nothing);
  RUN_TEST(test_a_negative_port_is_not_a_port);
  RUN_TEST(test_a_board_with_no_receiver_does_not_need_a_gps_port_number);
  return UNITY_END();
}
