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


// MagHeading: a field vector into a bearing, and the number that says whether
// to believe it. This is the arithmetic a bench cannot check without a
// reference field, and every mistake in it produces a heading that looks
// exactly like a working one:
//
//   * scoring the calibration on the best of three axes rather than the worse
//     of two reports a trustworthy compass after the board has been tipped end
//     over end, which swings Z through the whole field while X and Y — the pair
//     the bearing is computed from — have never moved;
//   * a divisor of 25 instead of 50 reports full calibration at half a turn;
//   * a sign slip in the tilt rotation gives a bearing that is steady, turns
//     with the board, and is wrong by tens of degrees at any angle off flat;
//   * dropping the absolute value in the tilt angle calls a board lying flat
//     on the bench 178 degrees tilted, because these parts read -1 g on Z face
//     up.
//
// The expected values here are worked out by hand from the convention, not
// read back from the code: x forward, y right, z down, bearing atan2(-y, x)
// clockwise from x, wrapped into 0-360.
#include <unity.h>
#include <math.h>
#include <stdint.h>
#include "../../src/sys/MagHeading.h"

// A horizontal field of 30 uT pointing along each of the four cardinals in the
// board's own frame, and the bearing each must produce under the convention.
// atan2(-y, x): +x is 0, -y is 90, -x is 180, +y is 270.
static void test_the_four_cardinals() {
  const float north[3] = {  30.0f,   0.0f, 0.0f };
  const float east[3]  = {   0.0f, -30.0f, 0.0f };
  const float south[3] = { -30.0f,   0.0f, 0.0f };
  const float west[3]  = {   0.0f,  30.0f, 0.0f };
  TEST_ASSERT_FLOAT_WITHIN(0.01f,   0.0f, MagHeading::flat(north).headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f,  90.0f, MagHeading::flat(east).headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 180.0f, MagHeading::flat(south).headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 270.0f, MagHeading::flat(west).headingDeg);
}

// Wrapped into 0-360 and never negative: atan2 returns -pi..pi, and a caller
// printing a bearing of -89 degrees has been handed a bug rather than a
// heading.
static void test_the_bearing_is_wrapped_not_signed() {
  for (int deg = 0; deg < 360; deg += 15) {
    const float rad = (float)deg * 3.14159265f / 180.0f;
    // the inverse of the convention: x = cos(bearing), y = -sin(bearing)
    const float v[3] = { 40.0f * cosf(rad), -40.0f * sinf(rad), 0.0f };
    const float got = MagHeading::flat(v).headingDeg;
    TEST_ASSERT_TRUE(got >= 0.0f);
    TEST_ASSERT_TRUE(got < 360.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, (float)deg, got);
  }
}

// A vertical component must not move a flat bearing: the whole point of taking
// the arctangent of x and y is that z is out of it.
static void test_the_vertical_component_does_not_move_a_flat_bearing() {
  const float level[3] = { 20.0f, -20.0f,   0.0f };
  const float dipped[3] = { 20.0f, -20.0f, -45.0f };
  TEST_ASSERT_FLOAT_WITHIN(0.01f, MagHeading::flat(level).headingDeg,
                                  MagHeading::flat(dipped).headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, MagHeading::flat(level).headingDeg);
}

// flat() reports no tilt and says it was not levelled — both facts, because a
// caller that cannot tell "held flat" from "no accelerometer" will present a
// guess as a measurement.
static void test_flat_reports_that_it_was_not_levelled() {
  const float v[3] = { 30.0f, 0.0f, 0.0f };
  const MagHeading::Bearing b = MagHeading::flat(v);
  TEST_ASSERT_FALSE(b.levelled);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, b.tiltDeg);
}

// ---------------------------------------------------------------------------
// Hard iron

// The offsets are the middle of the extremes, subtracted. A field that has
// swung between -10 and +70 on x has a centre of 30, so a reading of 70 is
// +40 once the board's own contribution is gone.
static void test_hard_iron_subtracts_the_centre_of_the_extremes() {
  const float mn[3] = { -10.0f, -50.0f, -200.0f };
  const float mx[3] = {  70.0f,  50.0f,  -40.0f };
  const float raw[3] = { 70.0f, 0.0f, -120.0f };
  float out[3];
  MagHeading::removeHardIron(raw, mn, mx, out);
  TEST_ASSERT_FLOAT_WITHIN(0.001f,  40.0f, out[0]);   // 70 - 30
  TEST_ASSERT_FLOAT_WITHIN(0.001f,   0.0f, out[1]);   // 0 - 0
  TEST_ASSERT_FLOAT_WITHIN(0.001f,   0.0f, out[2]);   // -120 - -120
}

// The Supreme's own case: a large constant offset on one axis is exactly what
// hard-iron removal exists for, and it must not leak into the bearing. The
// same field with a 120 uT offset on z, once centred, gives the same heading.
static void test_a_large_offset_does_not_move_the_bearing() {
  const float mn[3] = { -30.0f, -30.0f,  90.0f };
  const float mx[3] = {  30.0f,  30.0f, 150.0f };
  const float raw[3] = { 30.0f, 0.0f, 120.0f };
  float out[3];
  MagHeading::removeHardIron(raw, mn, mx, out);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, MagHeading::flat(out).headingDeg);
}

// ---------------------------------------------------------------------------
// The calibration score

// Fifty microtesla of spread on the weaker of x and y is a full turn, and a
// board that has not moved has none.
static void test_the_score_is_the_spread_over_fifty() {
  const float still_mn[3] = { 10.0f, 10.0f, 10.0f };
  const float still_mx[3] = { 10.0f, 10.0f, 10.0f };
  TEST_ASSERT_EQUAL_UINT8(0, MagHeading::calibrationScore(still_mn, still_mx));

  const float half_mn[3] = { -12.5f, -12.5f, 0.0f };
  const float half_mx[3] = {  12.5f,  12.5f, 0.0f };
  TEST_ASSERT_EQUAL_UINT8(50, MagHeading::calibrationScore(half_mn, half_mx));

  const float full_mn[3] = { -25.0f, -25.0f, 0.0f };
  const float full_mx[3] = {  25.0f,  25.0f, 0.0f };
  TEST_ASSERT_EQUAL_UINT8(100, MagHeading::calibrationScore(full_mn, full_mx));
}

// The load-bearing choice, and the one a bench cannot see: scored on the WORSE
// of x and y. A board tipped end over end swings z through the whole field and
// one of x or y with it, and must still report an uncalibrated compass —
// because the bearing is computed from the pair, and the pair has not both
// moved.
static void test_the_score_takes_the_worse_of_the_two_bearing_axes() {
  // x fully swung, y untouched: the best of three would say 100, the best of
  // the two would say 100, and the honest answer is 0.
  const float mn[3] = { -25.0f, 5.0f, -60.0f };
  const float mx[3] = {  25.0f, 5.0f,  60.0f };
  TEST_ASSERT_EQUAL_UINT8(0, MagHeading::calibrationScore(mn, mx));

  // and the other way round, so the test cannot pass by always reading x
  const float mn2[3] = { 5.0f, -25.0f, -60.0f };
  const float mx2[3] = { 5.0f,  25.0f,  60.0f };
  TEST_ASSERT_EQUAL_UINT8(0, MagHeading::calibrationScore(mn2, mx2));
}

// Z is not scored at all, however far it has swung: it is not one of the two
// axes a bearing turns on.
static void test_z_never_contributes_to_the_score() {
  const float mn[3] = { -12.5f, -12.5f, -1000.0f };
  const float mx[3] = {  12.5f,  12.5f,  1000.0f };
  TEST_ASSERT_EQUAL_UINT8(50, MagHeading::calibrationScore(mn, mx));
}

// Clamped at both ends. Above a full turn is still 100 — a field stronger than
// Earth's, or a magnet waved past, must not report 300 % — and reversed
// extremes, which is what an unwritten pair of arrays would decode to, must
// not wrap to a large unsigned number.
static void test_the_score_is_clamped_at_both_ends() {
  const float wide_mn[3] = { -500.0f, -500.0f, 0.0f };
  const float wide_mx[3] = {  500.0f,  500.0f, 0.0f };
  TEST_ASSERT_EQUAL_UINT8(100, MagHeading::calibrationScore(wide_mn, wide_mx));

  // max below min: a negative spread, and 0 is the only honest answer
  const float bad_mn[3] = {  10.0f,  10.0f, 0.0f };
  const float bad_mx[3] = { -10.0f, -10.0f, 0.0f };
  TEST_ASSERT_EQUAL_UINT8(0, MagHeading::calibrationScore(bad_mn, bad_mx));
}

// ---------------------------------------------------------------------------
// Tilt compensation

// Held flat, the tilt-compensated bearing must equal the flat one — otherwise
// the correction is adding error to the case it cannot improve. This is the
// test that found the reflection: fed gravity as -1 g on z, which is what
// these parts report lying face up, the shipped rotation returned 315 where
// the flat path returns 45. Both signs are asserted, because a fix that only
// worked for one convention would be the same bug facing the other way.
static void test_held_flat_the_corrected_bearing_matches_the_flat_one() {
  const float c[3] = { 25.0f, -25.0f, -40.0f };
  const float faceUp[3]   = { 0.0f, 0.0f, -1.0f };   // what these parts report
  const float downIsPlus[3] = { 0.0f, 0.0f, 1.0f };  // the textbook convention
  const float* const both[2] = { faceUp, downIsPlus };
  for (int i = 0; i < 2; i++) {
    const MagHeading::Bearing t = MagHeading::tilted(c, both[i]);
    TEST_ASSERT_TRUE(t.levelled);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, MagHeading::flat(c).headingDeg, t.headingDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 45.0f, t.headingDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, t.tiltDeg);
  }
}

// And the agreement is not a property of one vector. Across the compass, a
// board that is flat must read the same corrected as uncorrected — which is
// the invariant the reflection broke and the one a reader should be able to
// rely on.
static void test_flat_agreement_holds_all_the_way_round() {
  const float faceUp[3] = { 0.0f, 0.0f, -1.0f };
  for (int deg = 0; deg < 360; deg += 10) {
    const float rad = (float)deg * 3.14159265f / 180.0f;
    const float c[3] = { 35.0f * cosf(rad), -35.0f * sinf(rad), -20.0f };
    const MagHeading::Bearing t = MagHeading::tilted(c, faceUp);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, (float)deg, t.headingDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, MagHeading::flat(c).headingDeg, t.headingDeg);
  }
}

// The case that makes the correction worth its arithmetic. The field is
// horizontal and pointing along +x, so the true bearing is 0 whatever the
// board does. Roll the board 45 degrees about x: in the board's own frame the
// field acquires a y and z component, the uncorrected bearing swings away, and
// the corrected one must come back to 0.
static void test_a_rolled_board_still_reads_the_same_bearing() {
  const float roll = 45.0f * 3.14159265f / 180.0f;
  const float sr = sinf(roll), cr = cosf(roll);
  // A field of 30 uT along +x with a 40 uT downward dip, rotated into the
  // board's frame by the roll; gravity likewise.
  const float fx = 30.0f, fz = 40.0f;          // world: +x horizontal, +z down
  const float c[3] = { fx, fz * sr, fz * cr }; // rolled about x
  const float g[3] = { 0.0f, -sr, -cr };       // -1 g down, rolled with it
  const MagHeading::Bearing t = MagHeading::tilted(c, g);
  TEST_ASSERT_TRUE(t.levelled);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, t.headingDeg > 180.0f
                                       ? t.headingDeg - 360.0f : t.headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 45.0f, t.tiltDeg);
  // and the uncorrected bearing really is wrong here, so the test above is
  // measuring the correction rather than an identity
  TEST_ASSERT_TRUE(fabsf(MagHeading::flat(c).headingDeg) > 5.0f &&
                   fabsf(MagHeading::flat(c).headingDeg - 360.0f) > 5.0f);
}

// Tilt is an angle from flat and never a signed one: these parts read -1 g on
// z lying face up, and the signed form called a board flat on the bench 178
// degrees tilted.
static void test_tilt_is_the_same_either_way_up() {
  const float c[3] = { 30.0f, 0.0f, 0.0f };
  const float faceUp[3]   = { 0.0f, 0.0f, -1.0f };
  const float faceDown[3] = { 0.0f, 0.0f,  1.0f };
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, MagHeading::tilted(c, faceUp).tiltDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, MagHeading::tilted(c, faceDown).tiltDeg);
}

// On edge is ninety degrees of tilt, and the value is not allowed to fall out
// of acos's domain when the vector is a shade over unit length.
static void test_on_edge_is_ninety_degrees_and_the_domain_holds() {
  const float c[3] = { 30.0f, 0.0f, 0.0f };
  const float edge[3] = { 0.0f, -1.0f, 0.0f };
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 90.0f, MagHeading::tilted(c, edge).tiltDeg);

  const float over[3] = { 0.0f, 0.0f, -1.0001f };   // rounding, not physics
  const float tilt = MagHeading::tilted(c, over).tiltDeg;
  TEST_ASSERT_FALSE(isnan(tilt));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, tilt);
}

// A gravity vector too short to say which way is down is not gravity. The
// bearing falls back to the flat one and, crucially, reports itself unlevelled
// rather than claiming a correction it did not make.
static void test_no_usable_gravity_falls_back_and_says_so() {
  const float c[3] = { 25.0f, -25.0f, 0.0f };
  const float none[3] = { 0.0f, 0.0f, 0.0f };
  const float tiny[3] = { 0.02f, 0.0f, -0.05f };   // under the 0.1 g floor
  const float* const cases[2] = { none, tiny };
  for (int i = 0; i < 2; i++) {
    const MagHeading::Bearing b = MagHeading::tilted(c, cases[i]);
    TEST_ASSERT_FALSE(b.levelled);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, MagHeading::flat(c).headingDeg, b.headingDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, b.tiltDeg);
  }
}

// Gravity's magnitude must not matter, only its direction: the part may report
// in g or in something else entirely, and the rotation normalises first.
static void test_only_the_direction_of_gravity_matters() {
  const float c[3] = { 20.0f, -14.0f, -30.0f };
  const float one[3]  = { 0.3f, 0.0f, -0.95f };
  const float ten[3]  = { 3.0f, 0.0f, -9.5f };
  const MagHeading::Bearing a = MagHeading::tilted(c, one);
  const MagHeading::Bearing b = MagHeading::tilted(c, ten);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, a.headingDeg, b.headingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, a.tiltDeg,    b.tiltDeg);
}

// ---------------------------------------------------------------------------
// The magnitude, which is the "something magnetic is near" number

static void test_the_magnitude() {
  const float v[3] = { 3.0f, 4.0f, 0.0f };
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, MagHeading::magnitude(v));
  // the Supreme's own reading, from LilyGO's example output
  const float bench[3] = { 2.56f, -34.13f, -118.67f };
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 123.51f, MagHeading::magnitude(bench));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_four_cardinals);
  RUN_TEST(test_the_bearing_is_wrapped_not_signed);
  RUN_TEST(test_the_vertical_component_does_not_move_a_flat_bearing);
  RUN_TEST(test_flat_reports_that_it_was_not_levelled);
  RUN_TEST(test_hard_iron_subtracts_the_centre_of_the_extremes);
  RUN_TEST(test_a_large_offset_does_not_move_the_bearing);
  RUN_TEST(test_the_score_is_the_spread_over_fifty);
  RUN_TEST(test_the_score_takes_the_worse_of_the_two_bearing_axes);
  RUN_TEST(test_z_never_contributes_to_the_score);
  RUN_TEST(test_the_score_is_clamped_at_both_ends);
  RUN_TEST(test_held_flat_the_corrected_bearing_matches_the_flat_one);
  RUN_TEST(test_flat_agreement_holds_all_the_way_round);
  RUN_TEST(test_a_rolled_board_still_reads_the_same_bearing);
  RUN_TEST(test_tilt_is_the_same_either_way_up);
  RUN_TEST(test_on_edge_is_ninety_degrees_and_the_domain_holds);
  RUN_TEST(test_no_usable_gravity_falls_back_and_says_so);
  RUN_TEST(test_only_the_direction_of_gravity_matters);
  RUN_TEST(test_the_magnitude);
  return UNITY_END();
}
