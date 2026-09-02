// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// Grid math is the kind of code that looks right and points a search party
// at the wrong hillside. The anchor is a published reference: the White
// House gate at 38.8977 N, 77.0365 W is 18S UJ 22821 06997, cited widely
// enough to serve as truth. The other cases pin the parts that vary — an
// odd zone, the southern hemisphere, the Norway exception — to the digits
// a reader can check by eye: zone, band, and the shape of the string.

#include <unity.h>
#include <string.h>
#include <stdlib.h>
#include "Mgrs.h"

void setUp() {}
void tearDown() {}

static void test_the_published_reference_point_comes_out_right() {
  char out[32];
  TEST_ASSERT_TRUE(Mgrs::fromLatLon(38.8977, -77.0365, out, sizeof(out)));
  // Zone, band and the 100 km square must be exact; the metre digits may
  // round differently by a metre or two across implementations.
  TEST_ASSERT_EQUAL_STRING_LEN("18S UJ ", out, 7);
  int e = atoi(out + 7), n = atoi(out + 13);
  TEST_ASSERT_INT_WITHIN(3, 22821, e);
  TEST_ASSERT_INT_WITHIN(3, 6997, n);
}

static void test_stockholm_lands_in_its_zone_and_band() {
  char out[32];
  TEST_ASSERT_TRUE(Mgrs::fromLatLon(59.32514, 18.07105, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING_LEN("34V ", out, 4);
  TEST_ASSERT_EQUAL(18, (int)strlen(out));
}

static void test_the_southern_hemisphere_gets_its_band() {
  char out[32];
  TEST_ASSERT_TRUE(Mgrs::fromLatLon(-33.8688, 151.2093, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING_LEN("56H ", out, 4);
}

static void test_the_norway_exception_moves_the_zone() {
  char out[32];
  TEST_ASSERT_TRUE(Mgrs::fromLatLon(60.0, 5.0, out, sizeof(out)));   // Bergen-ish
  TEST_ASSERT_EQUAL_STRING_LEN("32V ", out, 4);
}

static void test_out_of_range_is_refused() {
  char out[32];
  TEST_ASSERT_FALSE(Mgrs::fromLatLon(87.0, 10.0, out, sizeof(out)));
  TEST_ASSERT_FALSE(Mgrs::fromLatLon(-85.0, 10.0, out, sizeof(out)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_published_reference_point_comes_out_right);
  RUN_TEST(test_stockholm_lands_in_its_zone_and_band);
  RUN_TEST(test_the_southern_hemisphere_gets_its_band);
  RUN_TEST(test_the_norway_exception_moves_the_zone);
  RUN_TEST(test_out_of_range_is_refused);
  return UNITY_END();
}
