// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The vectors are ones a reader verifies by hand: a degree of latitude is
// ~111.2 km due north anywhere; a degree of longitude on the equator is the
// same due east; the same point is zero. Plus the inbound telemetry parser
// proven as a round-trip against the very encoder it mirrors — if the two
// ever disagree, the mesh's positions are fiction.

#include <unity.h>
#include <string.h>
#include <math.h>
#include "GeoMath.h"
#include "Telemetry.h"

void setUp() {}
void tearDown() {}

static void test_a_degree_of_latitude_is_111_km_north() {
  TEST_ASSERT_FLOAT_WITHIN(0.3, 111.19, GeoMath::distanceKm(50.0, 10.0, 51.0, 10.0));
  TEST_ASSERT_FLOAT_WITHIN(0.5, 0.0, GeoMath::bearingDeg(50.0, 10.0, 51.0, 10.0));
}

static void test_a_degree_of_equator_longitude_is_111_km_east() {
  TEST_ASSERT_FLOAT_WITHIN(0.3, 111.19, GeoMath::distanceKm(0.0, 10.0, 0.0, 11.0));
  TEST_ASSERT_FLOAT_WITHIN(0.5, 90.0, GeoMath::bearingDeg(0.0, 10.0, 0.0, 11.0));
}

static void test_the_same_point_is_zero() {
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, GeoMath::distanceKm(59.3, 18.0, 59.3, 18.0));
}

static void test_south_and_west_read_back_right() {
  TEST_ASSERT_FLOAT_WITHIN(0.5, 180.0, GeoMath::bearingDeg(51.0, 10.0, 50.0, 10.0));
  TEST_ASSERT_FLOAT_WITHIN(0.5, 270.0, GeoMath::bearingDeg(0.0, 11.0, 0.0, 10.0));
}

static void test_the_position_round_trips_through_the_wire_shape() {
  Rns::Telemetry::Snapshot s = {};
  s.havePosition = true;
  s.latitude = 59.325140;
  s.longitude = 18.071050;
  s.altitudeM = 41.0f;
  s.accuracyM = 12.5f;
  s.positionAt = 1788379475;
  uint8_t buf[256];
  const size_t n = Rns::Telemetry::fields(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(buf, n, pp));
  // Exact in the wire's own unit: the format is fixed-point microdegrees,
  // so the comparison is integer equality, no float-precision waiver needed.
  TEST_ASSERT_EQUAL_INT32(59325140, (int32_t)llround(pp.latitude * 1e6));
  TEST_ASSERT_EQUAL_INT32(18071050, (int32_t)llround(pp.longitude * 1e6));
  TEST_ASSERT_FLOAT_WITHIN(0.01, 41.0, pp.altitudeM);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 12.5, pp.accuracyM);
  TEST_ASSERT_EQUAL_UINT64(1788379475, pp.positionAt);
}

static void test_a_document_with_no_position_says_so() {
  Rns::Telemetry::Snapshot s = {};
  s.haveBattery = true;
  s.batteryPercent = 80.0f;
  uint8_t buf[128];
  const size_t n = Rns::Telemetry::fields(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_FALSE(Rns::Telemetry::parsePosition(buf, n, pp));
  // And the codec is a codec: 0,0 parses — judging the null island is the
  // ingest's policy, which is where the trust and range gates live too.
  Rns::Telemetry::Snapshot z = {};
  z.havePosition = true;
  uint8_t zb[128];
  const size_t zn = Rns::Telemetry::fields(z, zb, sizeof(zb));
  TEST_ASSERT_TRUE(zn > 0);
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(zb, zn, pp));
}

static void test_a_southern_western_position_survives_the_sign() {
  Rns::Telemetry::Snapshot s = {};
  s.havePosition = true;
  s.latitude = -33.868800;
  s.longitude = -71.123456;
  uint8_t buf[128];
  const size_t n = Rns::Telemetry::fields(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(buf, n, pp));
  TEST_ASSERT_EQUAL_INT32(-33868800, (int32_t)llround(pp.latitude * 1e6));
  TEST_ASSERT_EQUAL_INT32(-71123456, (int32_t)llround(pp.longitude * 1e6));
}

// The interop shapes the first suite missed: Sideband ships the document
// packed to bytes inside a bin, alongside other fields, sometimes in a
// map16 — the exact shapes whose absence let a vacuously green round-trip
// hide a feature that was dead on air.
static void test_a_bin_wrapped_document_parses_like_sidebands() {
  Rns::Telemetry::Snapshot s = {};
  s.havePosition = true;
  s.latitude = 59.325140;
  s.longitude = 18.071050;
  uint8_t doc[128];
  const size_t dn = Rns::Telemetry::build(s, doc, sizeof(doc));
  TEST_ASSERT_TRUE(dn > 0);
  // fields = fixmap{ 4: "x" (a field before telemetry), 2: bin8<doc> }
  uint8_t wire[192];
  size_t w = 0;
  wire[w++] = 0x82;
  wire[w++] = 0x04;
  wire[w++] = 0xA1; wire[w++] = 'x';
  wire[w++] = 0x02;
  wire[w++] = 0xC4; wire[w++] = (uint8_t)dn;
  memcpy(wire + w, doc, dn); w += dn;
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(wire, w, pp));
  TEST_ASSERT_EQUAL_INT32(59325140, (int32_t)llround(pp.latitude * 1e6));
  TEST_ASSERT_EQUAL_INT32(18071050, (int32_t)llround(pp.longitude * 1e6));
}

static void test_a_map16_fields_dict_still_yields_the_position() {
  Rns::Telemetry::Snapshot s = {};
  s.havePosition = true;
  s.latitude = 1.5;
  s.longitude = 2.5;
  uint8_t doc[128];
  const size_t dn = Rns::Telemetry::build(s, doc, sizeof(doc));
  TEST_ASSERT_TRUE(dn > 0);
  // fields = map16 with 16 filler pairs before the telemetry entry.
  uint8_t wire[256];
  size_t w = 0;
  wire[w++] = 0xDE; wire[w++] = 0x00; wire[w++] = 17;
  for (uint8_t k = 0; k < 16; k++) { wire[w++] = (uint8_t)(0x10 + k); wire[w++] = 0xC0; }
  wire[w++] = 0x02;
  wire[w++] = 0xC4; wire[w++] = (uint8_t)dn;
  memcpy(wire + w, doc, dn); w += dn;
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(wire, w, pp));
  TEST_ASSERT_EQUAL_INT32(1500000, (int32_t)llround(pp.latitude * 1e6));
}

static void test_a_big_sibling_field_does_not_kill_the_position() {
  Rns::Telemetry::Snapshot s = {};
  s.havePosition = true;
  s.latitude = -5.0;
  s.longitude = 6.0;
  uint8_t doc[128];
  const size_t dn = Rns::Telemetry::build(s, doc, sizeof(doc));
  TEST_ASSERT_TRUE(dn > 0);
  // fields = fixmap{ 6: bin16<300 bytes> (an attachment), 2: bin8<doc> }
  static uint8_t wire[512];
  size_t w = 0;
  wire[w++] = 0x82;
  wire[w++] = 0x06;
  wire[w++] = 0xC5; wire[w++] = 0x01; wire[w++] = 0x2C;   // bin16, 300
  memset(wire + w, 0xAB, 300); w += 300;
  wire[w++] = 0x02;
  wire[w++] = 0xC4; wire[w++] = (uint8_t)dn;
  memcpy(wire + w, doc, dn); w += dn;
  Rns::Telemetry::ParsedPosition pp;
  TEST_ASSERT_TRUE(Rns::Telemetry::parsePosition(wire, w, pp));
  TEST_ASSERT_EQUAL_INT32(-5000000, (int32_t)llround(pp.latitude * 1e6));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_degree_of_latitude_is_111_km_north);
  RUN_TEST(test_a_degree_of_equator_longitude_is_111_km_east);
  RUN_TEST(test_the_same_point_is_zero);
  RUN_TEST(test_south_and_west_read_back_right);
  RUN_TEST(test_the_position_round_trips_through_the_wire_shape);
  RUN_TEST(test_a_document_with_no_position_says_so);
  RUN_TEST(test_a_southern_western_position_survives_the_sign);
  RUN_TEST(test_a_bin_wrapped_document_parses_like_sidebands);
  RUN_TEST(test_a_map16_fields_dict_still_yields_the_position);
  RUN_TEST(test_a_big_sibling_field_does_not_kill_the_position);
  UNITY_END();
  return 0;
}
