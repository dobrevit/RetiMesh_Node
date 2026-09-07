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


// EinkSleep: the order the e-paper controller has to be asked in. Worth
// pinning here rather than on a bench because every one of its mistakes is a
// silent one on the glass — the panel keeps showing the frame it already
// holds, so a controller left deaf, or an update computed against a
// difference that is no longer there, looks exactly like a node that has
// simply not changed its reading yet.
//
// What the sequence then does to the part — R10h with A[1:0] = 01, and the
// hardware reset the driver performs inside a mode change — is a register
// write against a panel that has to answer, and the only place that is proved
// is a bench. It is not faked here.
#include <unity.h>
#include <stdint.h>
#include "../../src/ui/EinkSleep.h"

// ---------------------------------------------------------------------------
// A panel nobody has blanked

// begin() resets the controller and shows a frame, so a freshly started panel
// is awake and its first update owes nothing beyond what the refresh policy
// asked for. The first frame after boot is a full one because the policy says
// so, not because this thinks a wake is outstanding.
static void test_starts_awake_and_owes_nothing() {
  EinkSleep s;
  TEST_ASSERT_FALSE(s.asleep());
  TEST_ASSERT_FALSE(s.resuming());

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_FALSE(p.reload);
  TEST_ASSERT_FALSE(p.full);
}

// The resting life of this panel: partial after partial, and a full one every
// few of them to clear the ghosting. None of that touches the controller's
// power state, so none of it may cost a reset.
static void test_ordinary_updates_never_reset() {
  EinkSleep s;
  for (int i = 0; i < 20; i++) {
    const bool wantFull = (i % 5) == 0;
    EinkSleep::Preamble p = s.beforeUpdate(wantFull);
    TEST_ASSERT_FALSE(p.reload);
    TEST_ASSERT_EQUAL(wantFull, p.full);
  }
}

// ---------------------------------------------------------------------------
// The blank the operator asked for

static void test_sleep_sends_once() {
  EinkSleep s;
  TEST_ASSERT_TRUE(s.sleep());          // the command goes out
  TEST_ASSERT_TRUE(s.asleep());
  // The screen edge is reached from a long press and from the page walker and
  // arrives repeatedly with the same answer. A sleeping controller would not
  // hear the second command anyway; the point is that it is not sent.
  TEST_ASSERT_FALSE(s.sleep());
  TEST_ASSERT_FALSE(s.sleep());
  TEST_ASSERT_TRUE(s.asleep());
}

// Waking sends nothing at all. The part leaves deep sleep only on a hardware
// reset, and that reset belongs to the update that follows: a wake with no
// frame behind it would reset and reconfigure a controller nobody then draws
// to.
static void test_wake_defers_the_reset_to_the_update() {
  EinkSleep s;
  s.sleep();
  TEST_ASSERT_TRUE(s.wake());
  TEST_ASSERT_FALSE(s.asleep());
  TEST_ASSERT_TRUE(s.resuming());       // still owed
  TEST_ASSERT_FALSE(s.wake());          // and owed once, not twice

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_TRUE(p.reload);
  TEST_ASSERT_TRUE(p.full);
  TEST_ASSERT_FALSE(s.resuming());
}

// The debt is paid by one update and not carried into the next. A panel that
// reset before every frame would flash black-white-black every five minutes
// for the rest of its life.
static void test_the_resume_is_paid_once() {
  EinkSleep s;
  s.sleep();
  s.wake();
  (void)s.beforeUpdate(false);

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_FALSE(p.reload);
  TEST_ASSERT_FALSE(p.full);
}

// Waking a panel that was never asleep is not a reason to reset it: the same
// edge delivers "not blank" on every ordinary pass through the page walker.
static void test_wake_without_sleep_owes_nothing() {
  EinkSleep s;
  TEST_ASSERT_FALSE(s.wake());
  TEST_ASSERT_FALSE(s.resuming());

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_FALSE(p.reload);
  TEST_ASSERT_FALSE(p.full);
}

// ---------------------------------------------------------------------------
// The frame that did not come through the screen edge

// A boot notice paints straight to the glass without asking the page model
// anything, and a store move can raise one long after boot. If that frame is
// the first thing a sleeping controller is given, it goes into a part that is
// not listening and the driver then waits on a BUSY line that never falls.
// So an update while asleep is itself a wake.
static void test_update_while_asleep_is_a_wake() {
  EinkSleep s;
  s.sleep();

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_TRUE(p.reload);
  TEST_ASSERT_TRUE(p.full);
  TEST_ASSERT_FALSE(s.asleep());
  TEST_ASSERT_FALSE(s.resuming());

  // And having self-healed, it is an ordinary panel again.
  EinkSleep::Preamble q = s.beforeUpdate(false);
  TEST_ASSERT_FALSE(q.reload);
  TEST_ASSERT_FALSE(q.full);
}

// The wake that follows such an update finds nothing left to do — the screen
// edge arrives late, after flush() has already paid the reset, and must not
// buy a second one.
static void test_late_wake_after_a_self_healed_update() {
  EinkSleep s;
  s.sleep();
  (void)s.beforeUpdate(false);
  TEST_ASSERT_FALSE(s.wake());

  EinkSleep::Preamble p = s.beforeUpdate(false);
  TEST_ASSERT_FALSE(p.reload);
  TEST_ASSERT_FALSE(p.full);
}

// ---------------------------------------------------------------------------
// The resume raises the update and never lowers it

// A full refresh already asked for stays a full refresh, and the reset is
// still owed: the controller is deaf either way.
static void test_resume_over_a_full_update() {
  EinkSleep s;
  s.sleep();
  s.wake();

  EinkSleep::Preamble p = s.beforeUpdate(true);
  TEST_ASSERT_TRUE(p.reload);
  TEST_ASSERT_TRUE(p.full);
}

// The whole cycle, over and over, because the state that matters is the one
// left behind: a node whose operator blanks and wakes the panel through an
// afternoon must cost exactly one reset per wake and no drift after the
// fifth.
static void test_repeated_cycles_cost_one_reset_each() {
  EinkSleep s;
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_TRUE(s.sleep());
    TEST_ASSERT_TRUE(s.wake());

    EinkSleep::Preamble first = s.beforeUpdate(false);
    TEST_ASSERT_TRUE(first.reload);
    TEST_ASSERT_TRUE(first.full);

    for (int j = 0; j < 4; j++) {
      EinkSleep::Preamble p = s.beforeUpdate(false);
      TEST_ASSERT_FALSE(p.reload);
      TEST_ASSERT_FALSE(p.full);
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_starts_awake_and_owes_nothing);
  RUN_TEST(test_ordinary_updates_never_reset);
  RUN_TEST(test_sleep_sends_once);
  RUN_TEST(test_wake_defers_the_reset_to_the_update);
  RUN_TEST(test_the_resume_is_paid_once);
  RUN_TEST(test_wake_without_sleep_owes_nothing);
  RUN_TEST(test_update_while_asleep_is_a_wake);
  RUN_TEST(test_late_wake_after_a_self_healed_update);
  RUN_TEST(test_resume_over_a_full_update);
  RUN_TEST(test_repeated_cycles_cost_one_reset_each);
  return UNITY_END();
}
