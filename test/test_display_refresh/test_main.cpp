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


// Refresh policy: what a page draws and what reaches the glass are not the
// same thing, and on e-paper the difference is the whole point.
#include <unity.h>
#include "../../src/ui/RefreshPolicy.h"

using Action = RefreshPolicy::Action;

// An OLED affords an update twice a second and never needs a full refresh.
static RefreshPolicy oled() { return RefreshPolicy(500, 0); }
// An e-paper panel: no more than one update every five seconds while somebody
// is watching a page, and a whole-panel refresh every five partial ones to
// clear the ghosting they leave behind.
static RefreshPolicy eink() { return RefreshPolicy(5000, 5); }

static void test_the_first_frame_always_reaches_the_glass() {
  // Whatever the previous firmware left on the panel is not something we can
  // compare against, so the first frame goes out even though nothing "changed".
  RefreshPolicy p = oled();
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(0, 0xAAAA));
}

static void test_glass_we_have_not_written_to_earns_a_full_refresh() {
  // On e-paper the first frame is the one that most needs a whole-panel pass:
  // what is on the glass belongs to the firmware before us, and a partial
  // update would leave it there as a ghost indefinitely.
  RefreshPolicy p = eink();
  TEST_ASSERT_EQUAL((int)Action::Full, (int)p.decide(0, 0xAAAA));
}

static void test_an_unchanged_frame_is_not_pushed() {
  RefreshPolicy p = oled();
  p.decide(0, 0xAAAA);
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(10000, 0xAAAA));
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(20000, 0xAAAA));
}

static void test_a_changed_frame_waits_for_what_the_panel_can_afford() {
  RefreshPolicy p = eink();
  TEST_ASSERT_EQUAL((int)Action::Full, (int)p.decide(0, 1));      // the first is always full
  // Changed, but three seconds after the last update on a panel that wants
  // five between them: it waits rather than flashing the screen.
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(3000, 2));
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(5000, 2));
}

static void test_a_value_that_flickers_does_not_drive_the_panel() {
  // A reading alternating between two values must not update the panel every
  // time it moves: the interval applies to changes as much as to repeats.
  RefreshPolicy p = eink();
  p.decide(0, 1);
  int pushes = 0;
  for (uint32_t t = 100; t <= 4900; t += 100)
    if (p.decide(t, t % 200 ? 2u : 3u) != Action::Skip) pushes++;
  TEST_ASSERT_EQUAL_INT(0, pushes);
}

static void test_the_ghosting_is_cleared_every_five_partial_updates() {
  // Counted in updates, not in minutes: the ghosting is left behind by the
  // partial updates themselves, so it is those that have to be counted.
  RefreshPolicy p = eink();
  TEST_ASSERT_EQUAL((int)Action::Full, (int)p.decide(0, 0));     // first frame
  uint32_t t = 0;
  for (int i = 1; i <= 5; i++) {
    t += 5000;
    TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(t, (uint32_t)i));
  }
  t += 5000;
  TEST_ASSERT_EQUAL((int)Action::Full, (int)p.decide(t, 99));    // the sixth clears it
  t += 5000;
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(t, 100));// and the count restarts
}

static void test_a_panel_left_alone_owes_no_full_refresh() {
  // An unchanging page produces no partial updates, so it accrues no
  // ghosting: hours of it must not add up to a flash nobody needed.
  RefreshPolicy p = eink();
  p.decide(0, 7);
  for (uint32_t t = 5000; t <= 3600000; t += 60000)
    TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(t, 7));
}

static void test_a_panel_that_never_needs_one_never_gets_a_full_refresh() {
  RefreshPolicy p = oled();
  p.decide(0, 7);
  for (uint32_t t = 1000; t <= 3600000; t += 60000)
    TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(t, 7));
  // Even through changes: an OLED has nothing to clear, so a full refresh is
  // never what it wants.
  for (uint32_t t = 3601000; t <= 3660000; t += 1000)
    TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(t, t));
}

static void test_the_resting_cadence_can_be_changed_for_an_attended_page() {
  // The node rests at five minutes; a page somebody turned to runs at five
  // seconds. Same policy, told which it is.
  RefreshPolicy p(300000, 5);
  p.decide(0, 1);
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(60000, 2));   // resting: not yet
  p.interval(5000);
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(60001, 2));
}

static void test_forgetting_makes_the_next_frame_go_out() {
  // The panel was switched off, or a boot notice painted over the page: what
  // we believe is on the glass is no longer true.
  RefreshPolicy p = oled();
  p.decide(0, 7);
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(5000, 7));
  p.forget();
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(5001, 7));
}

static void test_the_interval_does_not_hold_across_a_forget() {
  // Waking the panel must not wait out the panel's gap: the operator pressed
  // a button and is looking at it.
  RefreshPolicy p = eink();
  p.decide(0, 7);
  p.forget();
  // Full rather than partial, for the same reason as the first frame: after a
  // forget, what is on the glass is no longer something we know.
  TEST_ASSERT_EQUAL((int)Action::Full, (int)p.decide(1, 8));
}

static void test_a_press_does_not_wait_out_the_panels_gap() {
  // Turning the page is a person asking. On a panel that keeps five seconds
  // between updates, making them wait for the tick reads as a dropped press.
  RefreshPolicy p = eink();
  p.decide(0, 1);
  TEST_ASSERT_EQUAL((int)Action::Skip, (int)p.decide(200, 2));   // ordinary change: waits
  p.urgent();
  // Partial, not full: the glass is not unknown, it shows the page before
  // this one, so the press must not cost a whole-panel flash.
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(300, 3));
}

static void test_urgency_is_spent_on_the_frame_it_showed() {
  // One press, one frame out of turn — not a licence to ignore the gap for
  // everything that follows it.
  RefreshPolicy p = eink();
  p.decide(0, 1);
  p.urgent();
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(100, 2));
  TEST_ASSERT_EQUAL((int)Action::Skip,    (int)p.decide(200, 3));
}

static void test_the_clock_wrapping_does_not_freeze_the_panel() {
  // millis() wraps every 49 days. Unsigned differences carry through it, and
  // a soak run that spans one must not stop updating.
  RefreshPolicy p = eink();
  const uint32_t nearWrap = 0xFFFFFF00u;
  TEST_ASSERT_EQUAL((int)Action::Full,    (int)p.decide(nearWrap, 1));   // first frame is always full
  TEST_ASSERT_EQUAL((int)Action::Skip,    (int)p.decide(nearWrap + 1000, 2));
  TEST_ASSERT_EQUAL((int)Action::Partial, (int)p.decide(nearWrap + 6000, 2));   // wrapped
}

static void test_the_hash_tells_frames_apart_and_repeats_itself() {
  const uint8_t a[] = { 1, 2, 3, 4 };
  const uint8_t b[] = { 1, 2, 3, 5 };
  TEST_ASSERT_EQUAL_UINT32(RefreshPolicy::hash(a, sizeof(a)), RefreshPolicy::hash(a, sizeof(a)));
  TEST_ASSERT_NOT_EQUAL(RefreshPolicy::hash(a, sizeof(a)), RefreshPolicy::hash(b, sizeof(b)));
  // A one-bit difference anywhere in a frame has to be visible, because that
  // is a character that changed.
  uint8_t big[1024] = {0};
  const uint32_t clean = RefreshPolicy::hash(big, sizeof(big));
  big[900] = 0x01;
  TEST_ASSERT_NOT_EQUAL(clean, RefreshPolicy::hash(big, sizeof(big)));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_frame_always_reaches_the_glass);
  RUN_TEST(test_glass_we_have_not_written_to_earns_a_full_refresh);
  RUN_TEST(test_an_unchanged_frame_is_not_pushed);
  RUN_TEST(test_a_changed_frame_waits_for_what_the_panel_can_afford);
  RUN_TEST(test_a_value_that_flickers_does_not_drive_the_panel);
  RUN_TEST(test_the_ghosting_is_cleared_every_five_partial_updates);
  RUN_TEST(test_a_panel_left_alone_owes_no_full_refresh);
  RUN_TEST(test_the_resting_cadence_can_be_changed_for_an_attended_page);
  RUN_TEST(test_a_panel_that_never_needs_one_never_gets_a_full_refresh);
  RUN_TEST(test_forgetting_makes_the_next_frame_go_out);
  RUN_TEST(test_the_interval_does_not_hold_across_a_forget);
  RUN_TEST(test_a_press_does_not_wait_out_the_panels_gap);
  RUN_TEST(test_urgency_is_spent_on_the_frame_it_showed);
  RUN_TEST(test_the_clock_wrapping_does_not_freeze_the_panel);
  RUN_TEST(test_the_hash_tells_frames_apart_and_repeats_itself);
  return UNITY_END();
}
