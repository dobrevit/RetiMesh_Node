// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// Grid math is the kind of code that looks right and points a search party
// at the wrong hillside. The vectors here were produced by the mgrs C
// library (the GEOTRANS-derived reference implementation) on the bench, so
// every case pins the full string — zone, band, square and both metre
// pairs. A first draft trusted a half-remembered figure instead and failed
// its own correct implementation; a reference is only a reference when a
// tool produced it.

#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include "Mgrs.h"

void setUp() {}
void tearDown() {}

static void expectMgrs(double lat, double lon, const char* want) {
  char out[32];
  TEST_ASSERT_TRUE(Mgrs::fromLatLon(lat, lon, out, sizeof(out)));
  // Letters exact; the metre pairs within a stride of rounding.
  TEST_ASSERT_EQUAL_STRING_LEN(want, out, 7);
  TEST_ASSERT_INT_WITHIN(3, atoi(want + 7),  atoi(out + 7));
  TEST_ASSERT_INT_WITHIN(3, atoi(want + 13), atoi(out + 13));
}

static void test_the_reference_point_comes_out_right() {
  expectMgrs(38.8977, -77.0365, "18S UJ 23394 07395");
}

static void test_stockholm_lands_in_its_square() {
  expectMgrs(59.32514, 18.07105, "34V CL 33349 79922");
}

static void test_the_southern_hemisphere_gets_its_band() {
  expectMgrs(-33.8688, 151.2093, "56H LH 34368 50948");
}

static void test_the_norway_exception_moves_the_zone() {
  expectMgrs(60.0, 5.0, "32V KM 76979 58157");
}

static void test_out_of_range_is_refused() {
  char out[32];
  TEST_ASSERT_FALSE(Mgrs::fromLatLon(87.0, 10.0, out, sizeof(out)));
  TEST_ASSERT_FALSE(Mgrs::fromLatLon(-85.0, 10.0, out, sizeof(out)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_reference_point_comes_out_right);
  RUN_TEST(test_stockholm_lands_in_its_square);
  RUN_TEST(test_the_southern_hemisphere_gets_its_band);
  RUN_TEST(test_the_norway_exception_moves_the_zone);
  RUN_TEST(test_out_of_range_is_refused);
  return UNITY_END();
}
