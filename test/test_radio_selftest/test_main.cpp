// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// When the radio's boot self-test is skipped, and when it is not.
//
// The decision costs a transmission on a node whose battery is already too
// flat to finish a boot, so getting it wrong is expensive in exactly the
// situation it exists for — and neither direction is observable on a bench
// without a programmable supply and the patience to brown-out a board a
// hundred times. The rule is pure, so it is pinned here instead.

#include <unity.h>
#include <string.h>
#include "RadioSelfTestPolicy.h"

// A node that has never run this image has nothing stored, and NVS answers a
// missing key with the default the caller passes — NO_MARK. That must read as
// "run", not as a marker that happens to be zero.
static void test_a_node_with_no_marker_runs_the_test() {
  const uint32_t build = RadioSelfTest::buildMark("v1.2.3");
  TEST_ASSERT_FALSE_MESSAGE(RadioSelfTest::proven(RadioSelfTest::NO_MARK, build),
                            "nothing stored means the question has not been answered here");
}

// The whole point: the second boot of an image that already answered pays no
// airtime, however many times it happens. This is the brown-out loop.
static void test_the_same_build_is_only_proved_once() {
  const uint32_t build = RadioSelfTest::buildMark("v1.2.3");
  const uint32_t stored = RadioSelfTest::markAfter(true, build);
  TEST_ASSERT_TRUE_MESSAGE(RadioSelfTest::proven(stored, build),
                           "a build that passed does not have to pass again");
  // ...and it stays true across as many boots as the loop makes.
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_TRUE(RadioSelfTest::proven(stored, RadioSelfTest::buildMark("v1.2.3")));
}

// What the test proves is the board header's pin map, and that belongs to the
// image. A different image has to prove itself even on a node where its
// predecessor passed — this is the case that makes the marker safe to keep.
static void test_a_different_build_proves_itself_again() {
  const uint32_t stored = RadioSelfTest::markAfter(true, RadioSelfTest::buildMark("v1.2.3"));
  const char* others[] = { "v1.2.4", "v1.2.3-1-gdeadbee", "v1.2.3-dirty", "dev", "" };
  for (const char* v : others)
    TEST_ASSERT_FALSE_MESSAGE(RadioSelfTest::proven(stored, RadioSelfTest::buildMark(v)),
                              "another image must not inherit this one's verdict");
}

// A failure stores nothing. The board this test exists for is one whose
// interrupt pin is wrong, and recording that as proven would silence the only
// line in the boot log that names the fault — for ever, on that image.
static void test_a_failed_self_test_is_never_remembered() {
  const uint32_t build = RadioSelfTest::buildMark("v1.2.3");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(RadioSelfTest::NO_MARK,
                                   RadioSelfTest::markAfter(false, build),
                                   "a failure has nothing to remember");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(build, RadioSelfTest::markAfter(true, build),
                                   "a pass is remembered as this build");
  // ...and the next boot of that same build therefore runs it again.
  TEST_ASSERT_FALSE(RadioSelfTest::proven(RadioSelfTest::markAfter(false, build), build));
}

// The marker is only worth keeping if it is the same four bytes every boot of
// one binary, and different bytes for a different one. Both halves matter: a
// mark that drifted would re-pay the airtime for ever, and one that collided
// would skip a check the new image had never passed.
static void test_the_build_mark_is_stable_and_distinguishing() {
  TEST_ASSERT_EQUAL_UINT32(RadioSelfTest::buildMark("v0.0.9-35-g8465afd"),
                           RadioSelfTest::buildMark("v0.0.9-35-g8465afd"));
  const char* versions[] = { "dev", "v0.0.9", "v0.0.10", "v0.0.9-1-gaaaaaaa",
                             "v0.0.9-1-gbbbbbbb", "v0.0.9-dirty", "" };
  const size_t n = sizeof(versions) / sizeof(versions[0]);
  for (size_t i = 0; i < n; i++)
    for (size_t j = i + 1; j < n; j++)
      TEST_ASSERT_NOT_EQUAL_MESSAGE(RadioSelfTest::buildMark(versions[i]),
                                    RadioSelfTest::buildMark(versions[j]),
                                    "two firmware versions must not share a marker");
}

// NO_MARK is the "nothing stored" reading, so no build may ever produce it —
// including the empty string a build without git falls back through.
static void test_no_build_can_mint_the_empty_marker() {
  const char* versions[] = { "", "dev", "v1.0.0", "\x01", "a", "zzzzzzzzzzzz" };
  for (const char* v : versions)
    TEST_ASSERT_NOT_EQUAL_MESSAGE(RadioSelfTest::NO_MARK, RadioSelfTest::buildMark(v),
                                  "a real build must never look like an empty slot");
  // A null version string is the same case: nothing to hash, still not empty.
  TEST_ASSERT_NOT_EQUAL(RadioSelfTest::NO_MARK, RadioSelfTest::buildMark(nullptr));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_node_with_no_marker_runs_the_test);
  RUN_TEST(test_the_same_build_is_only_proved_once);
  RUN_TEST(test_a_different_build_proves_itself_again);
  RUN_TEST(test_a_failed_self_test_is_never_remembered);
  RUN_TEST(test_the_build_mark_is_stable_and_distinguishing);
  RUN_TEST(test_no_build_can_mint_the_empty_marker);
  return UNITY_END();
}
