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
//
// What the firmware hands buildMark() is the ELF SHA-256 out of the running
// image's application descriptor, so the fixtures here are 32-byte digests.

#include <unity.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "RadioSelfTestPolicy.h"

// A stand-in for one image's app_elf_sha256: 32 bytes, all different from each
// other, seeded so a test can name "another image" without inventing one.
struct Digest {
  uint8_t b[32];
  explicit Digest(uint8_t seed = 0) {
    for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)(seed * 31u + i * 7u + 1u);
  }
};

static uint32_t markOf(const Digest& d) { return RadioSelfTest::buildMark(d.b, sizeof(d.b)); }

// A node that has never run this image has nothing stored, and NVS answers a
// missing key with the default the caller passes — NO_MARK. That must read as
// "run", not as a marker that happens to be zero.
static void test_a_node_with_no_marker_runs_the_test() {
  const uint32_t image = markOf(Digest(1));
  TEST_ASSERT_FALSE_MESSAGE(RadioSelfTest::proven(RadioSelfTest::NO_MARK, image),
                            "nothing stored means the question has not been answered here");
}

// The whole point: the second boot of an image that already answered pays no
// airtime, however many times it happens. This is the brown-out loop.
static void test_the_same_image_is_only_proved_once() {
  const Digest d(1);
  const uint32_t stored = RadioSelfTest::markAfter(true, markOf(d));
  TEST_ASSERT_TRUE_MESSAGE(RadioSelfTest::proven(stored, markOf(d)),
                           "an image that passed does not have to pass again");
  // ...and it stays true across as many boots as the loop makes.
  for (int i = 0; i < 100; i++)
    TEST_ASSERT_TRUE(RadioSelfTest::proven(stored, markOf(Digest(1))));
}

// What the test proves is the board header's pin map, and that belongs to the
// image. A different image has to prove itself even on a node where its
// predecessor passed — this is the case that makes the marker safe to keep.
static void test_a_different_image_proves_itself_again() {
  const uint32_t stored = RadioSelfTest::markAfter(true, markOf(Digest(1)));
  for (uint8_t seed = 2; seed < 40; seed++)
    TEST_ASSERT_FALSE_MESSAGE(RadioSelfTest::proven(stored, markOf(Digest(seed))),
                              "another image must not inherit this one's verdict");
}

// The reason the identity is the binary and not FW_VERSION.
//
// A bring-up session is: flash, watch the self-test, change the pin map or the
// SPI wiring, flash again. `git describe --always --dirty` gives every one of
// those builds the same string — the tree was already dirty, so the value did
// not move (tools/fw_version.py) — and keyed to that string the second image
// would find the first one's marker and announce a pin it had never driven.
// Two builds differing by one edit differ in their ELF hash, and one changed
// byte anywhere in that hash has to be a different marker.
static void test_two_images_of_one_version_string_do_not_share_a_marker() {
  Digest a(1);
  const uint32_t first = RadioSelfTest::markAfter(true, markOf(a));
  for (size_t i = 0; i < sizeof(a.b); i++) {
    Digest b(1);
    b.b[i] ^= 0x01;                      // the next build, same version string
    TEST_ASSERT_FALSE_MESSAGE(RadioSelfTest::proven(first, markOf(b)),
                              "a rebuild must prove the wiring it was built for");
  }
}

// A failure stores nothing. The board this test exists for is one whose
// interrupt pin is wrong, and recording that as proven would silence the only
// line in the boot log that names the fault — for ever, on that image.
static void test_a_failed_self_test_is_never_remembered() {
  const uint32_t image = markOf(Digest(1));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(RadioSelfTest::NO_MARK,
                                   RadioSelfTest::markAfter(false, image),
                                   "a failure has nothing to remember");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(image, RadioSelfTest::markAfter(true, image),
                                   "a pass is remembered as this image");
  // ...and the next boot of that same image therefore runs it again.
  TEST_ASSERT_FALSE(RadioSelfTest::proven(RadioSelfTest::markAfter(false, image), image));
}

// The marker is only worth keeping if it is the same four bytes every boot of
// one binary, and different bytes for a different one. Both halves matter: a
// mark that drifted would re-pay the airtime for ever, and one that collided
// would skip a check the new image had never passed.
//
// The length is part of the question too: a caller that passed a prefix of the
// digest would be identifying images by their first few bytes.
static void test_the_image_mark_is_stable_and_distinguishing() {
  const Digest d(9);
  TEST_ASSERT_EQUAL_UINT32(markOf(d), markOf(Digest(9)));
  for (uint8_t i = 0; i < 60; i++)
    for (uint8_t j = (uint8_t)(i + 1); j < 60; j++)
      TEST_ASSERT_NOT_EQUAL_MESSAGE(markOf(Digest(i)), markOf(Digest(j)),
                                    "two images must not share a marker");
  for (size_t n = 1; n < sizeof(d.b); n++)
    TEST_ASSERT_NOT_EQUAL_MESSAGE(markOf(d), RadioSelfTest::buildMark(d.b, n),
                                  "the whole digest is the identity, not a prefix of it");
}

// NO_MARK is the "nothing stored" reading, so no image may ever produce it —
// including the degenerate inputs a caller could hand in by mistake.
static void test_no_image_can_mint_the_empty_marker() {
  for (uint8_t seed = 0; seed < 60; seed++)
    TEST_ASSERT_NOT_EQUAL_MESSAGE(RadioSelfTest::NO_MARK, markOf(Digest(seed)),
                                  "a real image must never look like an empty slot");
  // Nothing to hash — a zero length, an all-zero digest, or no pointer at all —
  // is still not an empty slot.
  const uint8_t zeros[32] = { 0 };
  TEST_ASSERT_NOT_EQUAL(RadioSelfTest::NO_MARK, RadioSelfTest::buildMark(zeros, sizeof(zeros)));
  TEST_ASSERT_NOT_EQUAL(RadioSelfTest::NO_MARK, RadioSelfTest::buildMark(zeros, 0));
  TEST_ASSERT_NOT_EQUAL(RadioSelfTest::NO_MARK, RadioSelfTest::buildMark(nullptr, 32));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_node_with_no_marker_runs_the_test);
  RUN_TEST(test_the_same_image_is_only_proved_once);
  RUN_TEST(test_a_different_image_proves_itself_again);
  RUN_TEST(test_two_images_of_one_version_string_do_not_share_a_marker);
  RUN_TEST(test_a_failed_self_test_is_never_remembered);
  RUN_TEST(test_the_image_mark_is_stable_and_distinguishing);
  RUN_TEST(test_no_image_can_mint_the_empty_marker);
  return UNITY_END();
}
