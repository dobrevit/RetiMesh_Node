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


// Whether a node will take an update handed to it. The refusals matter more
// than the acceptance: each one is a sentence somebody reads while standing
// under a pole, and the wrong one sends them looking for the wrong thing.
#include <unity.h>
#include <string.h>
#include "../../src/sys/OtaUpdatePlan.h"

using Ota::Stage;

static void test_a_node_that_can_take_an_update_says_nothing() {
  TEST_ASSERT_NULL(Ota::uploadRefusal(true, true, Stage::Idle));
  // and again once a previous attempt has finished
  TEST_ASSERT_NULL(Ota::uploadRefusal(true, true, Stage::Failed));
}

static void test_a_board_with_one_app_slot_is_told_before_the_card_is() {
  // Both are wrong, and only one of them is worth walking back to the van for.
  const char* why = Ota::uploadRefusal(false, false, Stage::Idle);
  TEST_ASSERT_NOT_NULL(why);
  TEST_ASSERT_NOT_NULL(strstr(why, "single app partition"));
}

static void test_a_missing_card_is_its_own_sentence() {
  const char* why = Ota::uploadRefusal(false, true, Stage::Idle);
  TEST_ASSERT_NOT_NULL(why);
  TEST_ASSERT_NOT_NULL(strstr(why, "card"));
}

static void test_a_second_upload_is_refused_while_one_is_running() {
  TEST_ASSERT_NOT_NULL(Ota::uploadRefusal(true, true, Stage::Receiving));
  TEST_ASSERT_NOT_NULL(Ota::uploadRefusal(true, true, Stage::Installing));
  // Staged is the gap between the last chunk landing and the installer
  // starting. A node that accepts an upload here deletes the bundle the
  // installer is a moment away from opening.
  TEST_ASSERT_NOT_NULL(Ota::uploadRefusal(true, true, Stage::Staged));
}

static void test_an_installed_update_refuses_another_until_the_restart() {
  // The node is on its way down into the new image. Accepting a second upload
  // here would write the far slot — which is the image it is running from
  // after the restart.
  const char* why = Ota::uploadRefusal(true, true, Stage::Installed);
  TEST_ASSERT_NOT_NULL(why);
  TEST_ASSERT_NOT_NULL(strstr(why, "restarting"));
}

static void test_every_stage_can_be_named() {
  const Stage all[] = { Stage::Idle, Stage::Receiving, Stage::Staged,
                        Stage::Installing, Stage::Installed, Stage::Failed };
  for (Stage s : all) {
    TEST_ASSERT_NOT_NULL(Ota::describe(s));
    TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", Ota::describe(s)));
  }
}

static void test_each_refusal_is_a_different_sentence() {
  const char* noSlot   = Ota::uploadRefusal(true,  false, Stage::Idle);
  const char* noCard   = Ota::uploadRefusal(false, true,  Stage::Idle);
  const char* busy     = Ota::uploadRefusal(true,  true,  Stage::Receiving);
  const char* done     = Ota::uploadRefusal(true,  true,  Stage::Installed);
  const char* all[] = { noSlot, noCard, busy, done };
  for (const char* r : all) TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(noSlot, noCard));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(noCard, busy));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(busy, done));
}

static void test_a_fresh_progress_reports_nothing_in_flight() {
  const Ota::Progress p;
  TEST_ASSERT_TRUE(p.stage == Stage::Idle);
  TEST_ASSERT_EQUAL_UINT32(0, p.received);
  TEST_ASSERT_EQUAL_UINT32(0, p.expected);
  TEST_ASSERT_EQUAL_STRING("", p.message);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_node_that_can_take_an_update_says_nothing);
  RUN_TEST(test_a_board_with_one_app_slot_is_told_before_the_card_is);
  RUN_TEST(test_a_missing_card_is_its_own_sentence);
  RUN_TEST(test_a_second_upload_is_refused_while_one_is_running);
  RUN_TEST(test_an_installed_update_refuses_another_until_the_restart);
  RUN_TEST(test_every_stage_can_be_named);
  RUN_TEST(test_each_refusal_is_a_different_sentence);
  RUN_TEST(test_a_fresh_progress_reports_nothing_in_flight);
  return UNITY_END();
}
