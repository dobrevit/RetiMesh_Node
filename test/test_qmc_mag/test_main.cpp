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


// QmcMag: the bytes written to a QMC6310 and the microtesla its counts are
// worth. Every mistake available here is silent and looks like success — a
// range field one value off scales every reading by four and still turns with
// the board; a mode value landing in the rate field suspends a part that goes
// on answering its address; a sensitivity transcribed from the wrong row of
// QST's table gives a heading that is right and a field strength that is not,
// which is the number an operator uses to decide whether something magnetic is
// sitting next to the node.
//
// The vectors that matter are not invented here. They are the readings
// LilyGO's own example produced from this exact part on the bench, quoted in
// docs/hardware.md: raw counts 96, -1280 and -4450 reported as 2.56, -34.13
// and -118.67 microtesla. A scale that fails those is wrong however tidy its
// arithmetic looks.
//
// What is not tested here is the I2C conversation: a register write to a part
// that has to answer is proved on a bench and nowhere else.
#include <unity.h>
#include <stdint.h>
#include <math.h>
#include "../../src/sys/QmcMag.h"

using namespace QmcMag;

// ---------------------------------------------------------------------------
// CTRL1: four fields in one byte

// The configuration this driver runs the part in, stated as a byte. If this
// changes, it changes deliberately: continuous, 200 Hz, no oversampling, no
// downsampling.
static void test_the_run_byte_is_the_verified_configuration() {
  TEST_ASSERT_EQUAL_HEX8(0x3F, kQmc6310Ctrl1Run);
  TEST_ASSERT_EQUAL_HEX8(0x08, kQmc6310Ctrl2Run);
}

static void test_mode_occupies_the_bottom_two_bits() {
  TEST_ASSERT_EQUAL_HEX8(0x00, ctrl1(Mode::Suspend,    Odr::Hz10, Osr::X8, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0x01, ctrl1(Mode::Normal,     Odr::Hz10, Osr::X8, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0x02, ctrl1(Mode::Single,     Odr::Hz10, Osr::X8, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0x03, ctrl1(Mode::Continuous, Odr::Hz10, Osr::X8, Dsr::X1));
}

static void test_each_field_has_its_own_bits() {
  // One field moved at a time, from the all-zero byte, so a field written into
  // a neighbour's bits fails here rather than on a bench.
  const uint8_t base = ctrl1(Mode::Suspend, Odr::Hz10, Osr::X8, Dsr::X1);
  TEST_ASSERT_EQUAL_HEX8(0x00, base);
  TEST_ASSERT_EQUAL_HEX8(0x0C, ctrl1(Mode::Suspend, Odr::Hz200, Osr::X8, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0x30, ctrl1(Mode::Suspend, Odr::Hz10,  Osr::X1, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0xC0, ctrl1(Mode::Suspend, Odr::Hz10,  Osr::X8, Dsr::X8));
}

// And the fields compose without carrying into each other.
static void test_the_fields_compose() {
  TEST_ASSERT_EQUAL_HEX8(0xFF, ctrl1(Mode::Continuous, Odr::Hz200, Osr::X1, Dsr::X8));
  TEST_ASSERT_EQUAL_HEX8(0x33, ctrl1(Mode::Continuous, Odr::Hz10,  Osr::X1, Dsr::X1));
  TEST_ASSERT_EQUAL_HEX8(0x0F, ctrl1(Mode::Continuous, Odr::Hz200, Osr::X8, Dsr::X1));
}

// Suspend is a mode value and not a separate byte: the driver writes
// kCtrl1Suspend to stop the part, and that has to be the same thing as asking
// for the suspend mode with everything else clear — or a resume would restore
// fields a suspend never cleared.
static void test_suspend_is_the_mode_with_nothing_else_set() {
  TEST_ASSERT_EQUAL_HEX8(kCtrl1Suspend,
                         ctrl1(Mode::Suspend, Odr::Hz10, Osr::X8, Dsr::X1));
}

// ---------------------------------------------------------------------------
// CTRL2: the range, and nothing else

static void test_range_occupies_bits_three_and_two() {
  TEST_ASSERT_EQUAL_HEX8(0x00, ctrl2(Range::G30));
  TEST_ASSERT_EQUAL_HEX8(0x04, ctrl2(Range::G12));
  TEST_ASSERT_EQUAL_HEX8(0x08, ctrl2(Range::G8));
  TEST_ASSERT_EQUAL_HEX8(0x0C, ctrl2(Range::G2));
}

// The self-test bit and the soft-reset bit must never appear in a run value: a
// run byte carrying bit 7 resets the part on every mode write, which is a
// compass that reports a first reading and then nothing.
static void test_a_run_value_carries_no_reset_and_no_self_test() {
  for (uint8_t r = 0; r < 4; r++) {
    const uint8_t v = ctrl2((Range)r);
    TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)(v & kSoftReset));
    TEST_ASSERT_EQUAL_HEX8(0x00, (uint8_t)(v & 0x40));
  }
}

// ---------------------------------------------------------------------------
// The scale, against the bench

// QST's table in LSB per gauss, turned into microtesla per count. Written out
// long-hand here rather than by calling the header's own arithmetic, so a
// transcription error in the header fails instead of being confirmed by
// itself.
static void test_microtesla_per_count_matches_the_sensitivity_table() {
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f,        utPerCount(Range::G30));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.04f,       utPerCount(Range::G12));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02666667f, utPerCount(Range::G8));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.00666667f, utPerCount(Range::G2));
}

// Ranges that halve should halve, which catches a row read off by one.
static void test_the_ranges_are_ordered_and_proportional() {
  TEST_ASSERT_TRUE(utPerCount(Range::G30) > utPerCount(Range::G12));
  TEST_ASSERT_TRUE(utPerCount(Range::G12) > utPerCount(Range::G8));
  TEST_ASSERT_TRUE(utPerCount(Range::G8)  > utPerCount(Range::G2));
  // 8 G is four times the range of 2 G, so its counts are four times as coarse.
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, utPerCount(Range::G8),
                           utPerCount(Range::G2) * 4.0f);
}

// The three readings LilyGO's example produced from this part on this board.
static void test_the_bench_vectors() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f,    2.56f, axisUt(   96, Range::G8));
  TEST_ASSERT_FLOAT_WITHIN(0.01f,  -34.13f, axisUt(-1280, Range::G8));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -118.67f, axisUt(-4450, Range::G8));
  // And the field magnitude those three make, which is the number that says
  // "something magnetic is near". The same bench line reported 123.51 uT for
  // this sample, so the three axes and the strength are one reading rather
  // than three borrowed from different moments.
  const float x = axisUt(96, Range::G8);
  const float y = axisUt(-1280, Range::G8);
  const float z = axisUt(-4450, Range::G8);
  const float mag = sqrtf(x * x + y * y + z * z);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 123.51f, mag);
}

// The same counts at the wrong range are wrong by a factor, which is exactly
// the mistake this file exists to catch — asserted so the vectors above cannot
// be satisfied by a scale that is merely close.
static void test_the_wrong_range_is_visibly_wrong() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.60f, axisUt(96, Range::G30));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.64f, axisUt(96, Range::G2));
}

// ---------------------------------------------------------------------------
// The output bytes

static void test_axis_counts_are_little_endian_and_signed() {
  TEST_ASSERT_EQUAL_INT16(     0, axisCounts(0x00, 0x00));
  TEST_ASSERT_EQUAL_INT16(    96, axisCounts(0x60, 0x00));
  TEST_ASSERT_EQUAL_INT16(  4660, axisCounts(0x34, 0x12));   // 0x1234, low byte first
  TEST_ASSERT_EQUAL_INT16(    -1, axisCounts(0xFF, 0xFF));
  TEST_ASSERT_EQUAL_INT16(-32768, axisCounts(0x00, 0x80));
  TEST_ASSERT_EQUAL_INT16( 32767, axisCounts(0xFF, 0x7F));
}

// Negative counts are most of a magnetometer's readings and the reason the
// cast has to be to a signed type: read as unsigned, -1280 becomes 64256 and
// the field reads 1713 uT, which looks like a fault in the part.
static void test_the_bench_vectors_from_their_bytes() {
  const int16_t y = axisCounts(0x00, 0xFB);                  // -1280
  TEST_ASSERT_EQUAL_INT16(-1280, y);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -34.13f, axisUt(y, Range::G8));
}

// ---------------------------------------------------------------------------
// The two parts are told apart by their chip ids, and confusing them is how
// this whole board cost a bring-up: 0x3c answered, was taken for a display,
// and swallowed every write.
static void test_the_chip_ids_differ() {
  TEST_ASSERT_EQUAL_HEX8(0x90, kChipIdQmc6309);
  TEST_ASSERT_EQUAL_HEX8(0x80, kChipIdQmc6310);
  TEST_ASSERT_TRUE(kChipIdQmc6309 != kChipIdQmc6310);
}

// The 6309's numbers are the M9's shipped constants. Pinned so that a change
// to this header cannot quietly reconfigure a board that has been working for
// months.
static void test_the_qmc6309_constants_are_unchanged() {
  TEST_ASSERT_EQUAL_HEX8(0xD3, kQmc6309Ctrl1Run);
  TEST_ASSERT_EQUAL_HEX8(0x03, kQmc6309Ctrl2Run);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0488f, kQmc6309UtPerCount);
}

// The status bits, which decide whether a sample is a measurement or the
// previous one read twice.
static void test_the_status_bits() {
  TEST_ASSERT_EQUAL_HEX8(0x01, kStatusDataReady);
  TEST_ASSERT_EQUAL_HEX8(0x02, kStatusOverflow);
  TEST_ASSERT_TRUE((kStatusDataReady & kStatusOverflow) == 0);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_run_byte_is_the_verified_configuration);
  RUN_TEST(test_mode_occupies_the_bottom_two_bits);
  RUN_TEST(test_each_field_has_its_own_bits);
  RUN_TEST(test_the_fields_compose);
  RUN_TEST(test_suspend_is_the_mode_with_nothing_else_set);
  RUN_TEST(test_range_occupies_bits_three_and_two);
  RUN_TEST(test_a_run_value_carries_no_reset_and_no_self_test);
  RUN_TEST(test_microtesla_per_count_matches_the_sensitivity_table);
  RUN_TEST(test_the_ranges_are_ordered_and_proportional);
  RUN_TEST(test_the_bench_vectors);
  RUN_TEST(test_the_wrong_range_is_visibly_wrong);
  RUN_TEST(test_axis_counts_are_little_endian_and_signed);
  RUN_TEST(test_the_bench_vectors_from_their_bytes);
  RUN_TEST(test_the_chip_ids_differ);
  RUN_TEST(test_the_qmc6309_constants_are_unchanged);
  RUN_TEST(test_the_status_bits);
  return UNITY_END();
}
