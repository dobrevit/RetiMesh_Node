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


// How far back an update may take this node. The cases that matter are the
// ones a device would only show you by refusing its next update months later:
// an install that never booted, and a version that tries to move the floor
// backwards.
#include <unity.h>
#include <map>
#include <string>
#include "../../src/sys/OtaFloor.h"
#include "../../src/sys/OtaVerify.h"

// A store with the four operations Floor asks for, and a record of the order
// they were called in — the ordering is the part that only fails after a power
// cut, so it is asserted rather than assumed.
struct FakeStore {
  std::map<std::string, uint32_t> values;
  std::string log;

  bool has(const char* k) const { return values.count(k) != 0; }
  uint32_t get(const char* k, uint32_t fallback) const {
    auto it = values.find(k);
    return it == values.end() ? fallback : it->second;
  }
  void put(const char* k, uint32_t v) { values[k] = v; log += std::string("put:") + k + ";"; }
  void drop(const char* k) { values.erase(k); log += std::string("drop:") + k + ";"; }
};

using Floor = Ota::Floor<FakeStore>;

// --- the decision on its own -----------------------------------------------

static void test_a_boot_with_nothing_staged_changes_nothing() {
  const Ota::Settlement s = Ota::settle(7, false, 0, true);
  TEST_ASSERT_EQUAL_UINT32(7, s.floor);
  TEST_ASSERT_FALSE(s.advanced);
  TEST_ASSERT_FALSE(s.clearStaged);
}

static void test_an_image_that_booted_raises_the_floor_to_itself() {
  const Ota::Settlement s = Ota::settle(7, true, 9, true);
  TEST_ASSERT_EQUAL_UINT32(9, s.floor);
  TEST_ASSERT_TRUE(s.advanced);
  TEST_ASSERT_TRUE(s.clearStaged);
}

static void test_an_image_that_was_rolled_back_leaves_the_floor_alone() {
  // The case the whole design turns on: writing an update must not raise the
  // floor, or a node that failed to boot it would refuse the fixed build.
  const Ota::Settlement s = Ota::settle(7, true, 9, false);
  TEST_ASSERT_EQUAL_UINT32(7, s.floor);
  TEST_ASSERT_FALSE(s.advanced);
  TEST_ASSERT_TRUE(s.clearStaged);   // but the stale record goes
}

static void test_the_floor_never_moves_backwards() {
  const Ota::Settlement s = Ota::settle(9, true, 4, true);
  TEST_ASSERT_EQUAL_UINT32(9, s.floor);
  TEST_ASSERT_FALSE(s.advanced);
  TEST_ASSERT_TRUE(s.clearStaged);
}

static void test_reinstalling_the_running_version_is_not_an_advance() {
  const Ota::Settlement s = Ota::settle(9, true, 9, true);
  TEST_ASSERT_EQUAL_UINT32(9, s.floor);
  TEST_ASSERT_FALSE(s.advanced);
}

static void test_a_first_ever_update_advances_from_nothing() {
  const Ota::Settlement s = Ota::settle(0, true, 1, true);
  TEST_ASSERT_EQUAL_UINT32(1, s.floor);
  TEST_ASSERT_TRUE(s.advanced);
}

// --- and what gets remembered ----------------------------------------------

static void test_a_node_that_has_never_updated_accepts_any_version() {
  FakeStore store; Floor floor(store);
  TEST_ASSERT_EQUAL_UINT32(0, floor.accepted());
  TEST_ASSERT_FALSE(floor.haveStaged());
}

static void test_a_successful_update_is_remembered_across_the_reboot() {
  FakeStore store; Floor floor(store);
  floor.stage(12);
  TEST_ASSERT_TRUE(floor.haveStaged());

  Floor next(store);                       // the boot after the install
  const Ota::Settlement s = next.confirm(true);
  TEST_ASSERT_TRUE(s.advanced);
  TEST_ASSERT_EQUAL_UINT32(12, next.accepted());
  TEST_ASSERT_FALSE(next.haveStaged());    // and does not settle twice
}

static void test_the_floor_is_written_before_the_staged_record_is_dropped() {
  // Losing power between the two writes must be survivable. A staged record
  // that outlives its floor settles to the same answer next boot; a floor that
  // never got written while the record was dropped loses the advance for good.
  FakeStore store; Floor floor(store);
  floor.stage(5);
  store.log.clear();
  floor.confirm(true);
  TEST_ASSERT_EQUAL_STRING("put:ota_floor;drop:ota_staged;", store.log.c_str());
}

static void test_a_rollback_forgets_the_staged_version_without_using_it() {
  FakeStore store; Floor floor(store);
  store.put("ota_floor", 3);
  floor.stage(8);
  const Ota::Settlement s = floor.confirm(false);
  TEST_ASSERT_FALSE(s.advanced);
  TEST_ASSERT_EQUAL_UINT32(3, floor.accepted());
  TEST_ASSERT_FALSE(floor.haveStaged());
}

static void test_settling_twice_is_harmless() {
  FakeStore store; Floor floor(store);
  floor.stage(6);
  floor.confirm(true);
  const Ota::Settlement again = floor.confirm(true);
  TEST_ASSERT_FALSE(again.advanced);
  TEST_ASSERT_EQUAL_UINT32(6, floor.accepted());
}

// --- the policy the floor feeds --------------------------------------------

static void test_a_policy_carries_the_firmwares_own_roots_and_board() {
  const FirmwareManifest::Policy p = Ota::policyFor(1966080, 12);
  TEST_ASSERT_EQUAL_UINT32(Ota::rootCount(), (uint32_t)p.rootCount);
  TEST_ASSERT_EQUAL_PTR(Ota::roots(), p.roots);
  TEST_ASSERT_EQUAL_STRING(OTA_BOARD_ID, p.board);
  TEST_ASSERT_EQUAL_UINT32(1966080, p.slotSize);
  TEST_ASSERT_EQUAL_UINT32(12, p.acceptedVersion);
}

static void test_the_floor_a_node_reached_is_what_the_policy_enforces() {
  FakeStore store; Floor floor(store);
  floor.stage(20);
  floor.confirm(true);
  TEST_ASSERT_EQUAL_UINT32(20, Ota::policyFor(1966080, floor.accepted()).acceptedVersion);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_boot_with_nothing_staged_changes_nothing);
  RUN_TEST(test_an_image_that_booted_raises_the_floor_to_itself);
  RUN_TEST(test_an_image_that_was_rolled_back_leaves_the_floor_alone);
  RUN_TEST(test_the_floor_never_moves_backwards);
  RUN_TEST(test_reinstalling_the_running_version_is_not_an_advance);
  RUN_TEST(test_a_first_ever_update_advances_from_nothing);
  RUN_TEST(test_a_node_that_has_never_updated_accepts_any_version);
  RUN_TEST(test_a_successful_update_is_remembered_across_the_reboot);
  RUN_TEST(test_the_floor_is_written_before_the_staged_record_is_dropped);
  RUN_TEST(test_a_rollback_forgets_the_staged_version_without_using_it);
  RUN_TEST(test_settling_twice_is_harmless);
  RUN_TEST(test_a_policy_carries_the_firmwares_own_roots_and_board);
  RUN_TEST(test_the_floor_a_node_reached_is_what_the_policy_enforces);
  return UNITY_END();
}
