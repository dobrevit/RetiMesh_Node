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


// What the status row shows for a version too long to print whole.
#include <unity.h>
#include "../../src/ui/VersionLabel.h"

static const char* fit(const char* version, size_t room) {
  static char out[64];
  VersionLabel::fit(version, room, out, sizeof(out));
  return out;
}

static void test_a_version_that_fits_is_shown_whole() {
  TEST_ASSERT_EQUAL_STRING("v0.0.9", fit("v0.0.9", 21));
  TEST_ASSERT_EQUAL_STRING("dev", fit("dev", 21));
  // Exactly the room available is still whole.
  TEST_ASSERT_EQUAL_STRING("v0.0.9", fit("v0.0.9", 6));
}

static void test_describe_output_falls_back_to_the_commit() {
  // 18 characters into the 10 a status row has left beside an address: the
  // commit is the part that answers "which build is this".
  TEST_ASSERT_EQUAL_STRING("g8465afd", fit("v0.0.9-35-g8465afd", 10));
}

static void test_a_dirty_tree_keeps_its_mark() {
  // Which commit it came from is half the answer when there were uncommitted
  // changes on top of it, so the star costs the one column it is worth.
  TEST_ASSERT_EQUAL_STRING("g8465afd*", fit("v0.0.9-35-g8465afd-dirty", 10));
  // And the whole thing still wins where it fits.
  TEST_ASSERT_EQUAL_STRING("v0.0.9-35-g8465afd-dirty", fit("v0.0.9-35-g8465afd-dirty", 41));
}

static void test_the_dirty_flag_is_never_mistaken_for_the_commit() {
  // "-dirty" is the last component, so a naive "take what follows the last
  // dash" would show the word dirty and no commit at all.
  const char* out = fit("v0.0.9-35-g8465afd-dirty", 10);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(out, "dirty"));
  TEST_ASSERT_EQUAL_STRING("g8465afd*", out);
}

static void test_cis_form_falls_back_to_its_sha() {
  // CI writes dev-<sha7>, which has no "-g" to find.
  TEST_ASSERT_EQUAL_STRING("8465afd", fit("dev-8465afd", 8));
  TEST_ASSERT_EQUAL_STRING("dev-8465afd", fit("dev-8465afd", 21));
}

static void test_a_row_with_no_room_says_nothing() {
  // A fragment of a hash identifies nothing, so a panel too narrow for even
  // the commit shows no version rather than a misleading piece of one.
  TEST_ASSERT_EQUAL_STRING("", fit("v0.0.9-35-g8465afd", 5));
  TEST_ASSERT_EQUAL_STRING("", fit("v0.0.9-35-g8465afd", 0));
}

static void test_nothing_is_written_past_the_buffer() {
  char out[4] = {'x', 'x', 'x', 'x'};
  const size_t n = VersionLabel::fit("v0.0.9-35-g8465afd", 21, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, n);          // will not fit in four bytes
  TEST_ASSERT_EQUAL_CHAR('\0', out[0]);    // and says so rather than overrunning
}

static void test_junk_and_absence_are_survivable() {
  TEST_ASSERT_EQUAL_STRING("", fit(nullptr, 21));
  TEST_ASSERT_EQUAL_STRING("", fit("", 21));
  // Anything that fits is shown as it is, however odd it looks: this only
  // reduces a version when the row cannot hold it, and a build that calls
  // itself "-" is reported rather than second-guessed.
  TEST_ASSERT_EQUAL_STRING("-", fit("-", 21));
  TEST_ASSERT_EQUAL_STRING("-g", fit("-g", 21));
  // It is the reducing path that has to cope with nonsense: no commit to
  // find, so nothing to show.
  TEST_ASSERT_EQUAL_STRING("", fit("----------", 3));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_version_that_fits_is_shown_whole);
  RUN_TEST(test_describe_output_falls_back_to_the_commit);
  RUN_TEST(test_a_dirty_tree_keeps_its_mark);
  RUN_TEST(test_the_dirty_flag_is_never_mistaken_for_the_commit);
  RUN_TEST(test_cis_form_falls_back_to_its_sha);
  RUN_TEST(test_a_row_with_no_room_says_nothing);
  RUN_TEST(test_nothing_is_written_past_the_buffer);
  RUN_TEST(test_junk_and_absence_are_survivable);
  return UNITY_END();
}
