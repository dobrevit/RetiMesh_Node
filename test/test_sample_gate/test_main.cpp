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


// Sample gate: the shared "is a periodic sample due" rule the battery
// readers ration their hardware access with. The property that matters most
// is the one whose absence shipped as a defect: the first ask must sample,
// or every caller reads zero-initialised state until the first interval has
// been waited out after boot.
#include <unity.h>
#include <stdint.h>
#include "../../src/sys/SampleGate.h"

static const uint32_t kInterval = 10000;   // BATTERY_SAMPLE_MS's shape

static void test_the_first_ask_is_due_even_at_time_zero() {
  // millis() is 0 at the instant the clock starts, and the gate must not
  // read "last sampled at 0" into that: the boot-time ask is the one every
  // caller — glyph, console, portal, telemetry — is waiting on.
  SampleGate g(kInterval);
  TEST_ASSERT_TRUE(g.due(0));
}

static void test_not_due_again_within_the_interval() {
  SampleGate g(kInterval);
  TEST_ASSERT_TRUE(g.due(0));
  TEST_ASSERT_FALSE(g.due(1));
  TEST_ASSERT_FALSE(g.due(kInterval / 2));
  TEST_ASSERT_FALSE(g.due(kInterval - 1));
}

static void test_due_again_after_the_interval() {
  SampleGate g(kInterval);
  TEST_ASSERT_TRUE(g.due(0));
  TEST_ASSERT_TRUE(g.due(kInterval));
  // And a due answer starts the next interval from itself.
  TEST_ASSERT_FALSE(g.due(kInterval + 1));
  TEST_ASSERT_TRUE(g.due(2 * kInterval));
}

static void test_the_interval_holds_across_a_millis_wrap() {
  // millis() wraps every 49.7 days, which a solar node lives through many
  // times. Last asked just before the wrap; the moments just after it are
  // inside the interval, and the moment one interval later is due.
  SampleGate g(kInterval);
  const uint32_t nearWrap = UINT32_MAX - 100;
  TEST_ASSERT_TRUE(g.due(nearWrap));
  TEST_ASSERT_FALSE(g.due(UINT32_MAX));            // 100 ms on
  TEST_ASSERT_FALSE(g.due(50));                    // 151 ms on, wrapped
  TEST_ASSERT_FALSE(g.due(kInterval - 102));       // one tick short
  TEST_ASSERT_TRUE(g.due(kInterval - 101));        // exactly the interval
}

static void test_a_primed_gate_holds_its_interval() {
  // begin() reads the hardware itself before any task can ask; priming
  // records that, so the first ask does not sample a second time — but the
  // interval still runs from the priming, not from the first ask.
  SampleGate g(kInterval);
  g.prime(500);
  TEST_ASSERT_FALSE(g.due(501));
  TEST_ASSERT_FALSE(g.due(500 + kInterval - 1));
  TEST_ASSERT_TRUE(g.due(500 + kInterval));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_ask_is_due_even_at_time_zero);
  RUN_TEST(test_not_due_again_within_the_interval);
  RUN_TEST(test_due_again_after_the_interval);
  RUN_TEST(test_the_interval_holds_across_a_millis_wrap);
  RUN_TEST(test_a_primed_gate_holds_its_interval);
  return UNITY_END();
}
