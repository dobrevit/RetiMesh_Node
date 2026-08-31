// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// What the node says about itself, read back with the same decoder it uses on
// what arrives (LxmfFormat.h). A telemetry value in the wrong msgpack type or
// the wrong shape is not rendered wrongly by a client — it is dropped, so the
// only way to know a field works is to take it apart again.
//
// The shapes are Sideband's and were confirmed against its own Telemeter:
// tools/telemetry_check.py builds this document and reads it back with the
// real class. That needs the app's source, so it is a bench check; what is
// here is the part CI can hold.

#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "Telemetry.h"
#include "LxmfFormat.h"

using namespace Rns;
using Rns::Telemetry::Snapshot;

// A node with everything: a fix, a charger it can see, a signal.
static Snapshot full() {
  Snapshot s;
  s.utc = 1767225600;
  snprintf(s.information, sizeof(s.information), "RetiMesh Node v0.1.0 (LilyGO T3-S3)");
  s.haveBattery = true; s.batteryPercent = 87.5f; s.charging = true; s.chargeKnown = true;
  s.havePosition = true; s.latitude = 42.6977; s.longitude = 23.3219;
  s.altitudeM = 595.0f; s.speedKmh = 0.0f; s.accuracyM = 7.5f; s.positionAt = 1767225590;
  s.haveSignal = true; s.rssi = -104.0f; s.snr = 8.75f; s.quality = 62;
  s.haveProcessor = true; s.cpuHz = 240000000;
  s.haveMemory = true; s.heapCapacity = 327680; s.heapUsed = 228164;
  s.haveStorage = true; s.flashCapacity = 3145728; s.flashUsed = 1751662;
  return s;
}

// Find one sensor's reading in the document, by id.
static bool sensor(const uint8_t* doc, size_t n, uint32_t sid,
                   const uint8_t*& val, size_t& valLen) {
  return lxmfField(doc, n, sid, val, valLen);      // a telemetry document is a fields map
}

static void test_a_document_holds_every_sensor_the_node_has() {
  uint8_t buf[512];
  const Snapshot s = full();
  const size_t n = Telemetry::build(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  // The header says how many pairs follow, and it is written from the members
  // actually emitted rather than from a second list of conditions.
  TEST_ASSERT_EQUAL_HEX8(0x88, buf[0]);

  const uint32_t expect[] = { Telemetry::kSidTime, Telemetry::kSidInformation,
                              Telemetry::kSidBattery, Telemetry::kSidLocation,
                              Telemetry::kSidPhysicalLink, Telemetry::kSidProcessor,
                              Telemetry::kSidRam, Telemetry::kSidNvm };
  for (uint32_t sid : expect) {
    const uint8_t* v = nullptr; size_t vl = 0;
    char why[48]; snprintf(why, sizeof(why), "sensor 0x%02X missing", (unsigned)sid);
    TEST_ASSERT_TRUE_MESSAGE(sensor(buf, n, sid, v, vl), why);
  }
}

// A board that cannot answer a question does not answer it. A fabricated
// reading is worse than a missing one, because a missing one is visibly so.
static void test_a_board_says_nothing_about_what_it_cannot_measure() {
  Snapshot s;
  s.utc = 1767225600;
  uint8_t buf[512];
  const size_t n = Telemetry::build(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0x81, buf[0]);            // one pair, and it is the clock
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(buf, n, Telemetry::kSidTime, v, vl));
  TEST_ASSERT_FALSE(sensor(buf, n, Telemetry::kSidBattery, v, vl));
  TEST_ASSERT_FALSE(sensor(buf, n, Telemetry::kSidLocation, v, vl));
  TEST_ASSERT_FALSE(sensor(buf, n, Telemetry::kSidRam, v, vl));
}

// Information is msgpack str. The announce display name is bin. Same project,
// opposite answers, and each is silently dropped if given the other.
static void test_information_is_a_string_and_not_bytes() {
  uint8_t buf[512];
  const size_t n = Telemetry::build(full(), buf, sizeof(buf));
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(buf, n, Telemetry::kSidInformation, v, vl));
  const uint8_t t = v[0];
  TEST_ASSERT_TRUE_MESSAGE((t & 0xE0) == 0xA0 || t == 0xD9 || t == 0xDA,
                           "Information must be msgpack str, not bin");
}

// A position is packed as bytes, big-endian fixed point — struct.pack("!i")
// on the other side — not as msgpack numbers. Every member is checked, not
// only the first: the widths and the signedness differ between them, and
// altitude is the one that matters most because a node can sit below sea
// level and the field is signed where speed and accuracy are not.
static void test_a_position_is_fixed_point_bytes_and_survives_the_trip() {
  uint8_t buf[512];
  Snapshot s = full();
  s.altitudeM = -20.5f;                              // Amsterdam, or a mine
  const size_t n = Telemetry::build(s, buf, sizeof(buf));
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(buf, n, Telemetry::kSidLocation, v, vl));
  TEST_ASSERT_EQUAL_HEX8(0x97, v[0]);                // fixarray of seven

  // Each member is bin of a fixed width; walk them and check value and size.
  struct { size_t width; int64_t want; bool sign; } expect[] = {
    { 4, 42697700, true },      // latitude  x 1e6
    { 4, 23321900, true },      // longitude x 1e6
    { 4, -2050,    true },      // altitude  x 1e2, signed
    { 4, 0,        false },     // speed     x 1e2, unsigned
    { 4, 0,        true },      // bearing   x 1e2, nothing here measures one
    { 2, 750,      false },     // accuracy  x 1e2, unsigned
  };
  size_t at = 1;
  for (size_t k = 0; k < 6; k++) {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xC4, v[at], "each member is msgpack bin");
    TEST_ASSERT_EQUAL_size_t(expect[k].width, v[at + 1]);
    int64_t got = 0;
    for (size_t b = 0; b < expect[k].width; b++) got = (got << 8) | v[at + 2 + b];
    if (expect[k].sign) {                            // sign-extend what was signed
      const int64_t top = (int64_t)1 << (expect[k].width * 8 - 1);
      if (got & top) got -= (int64_t)1 << (expect[k].width * 8);
    }
    char why[40]; snprintf(why, sizeof(why), "location member %u", (unsigned)k);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(expect[k].want, got, why);
    at += 2 + expect[k].width;
  }
}

// A reading too large for its field is held to that field, not wrapped. An
// accuracy estimate of 700 m becoming a confident 44.6 m is worse than none.
static void test_a_reading_too_large_for_its_field_is_clamped_not_wrapped() {
  uint8_t buf[512];
  Snapshot s = full();
  s.accuracyM = 900.0f;                              // x100 overflows a uint16
  const size_t n = Telemetry::build(s, buf, sizeof(buf));
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(buf, n, Telemetry::kSidLocation, v, vl));
  size_t at = 1;
  for (size_t k = 0; k < 5; k++) at += 2 + v[at + 1];
  const uint32_t accuracy = ((uint32_t)v[at + 2] << 8) | v[at + 3];
  TEST_ASSERT_EQUAL_UINT32(65535, accuracy);
}

// Charging is nil where the board cannot see its charger, not false. "Not
// charging" sends somebody looking for a fault in a working cable (Power.h).
static void test_a_charger_a_board_cannot_see_is_not_reported_as_absent() {
  uint8_t buf[512];
  Snapshot s = full();
  s.chargeKnown = false;
  const size_t n = Telemetry::build(s, buf, sizeof(buf));
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(buf, n, Telemetry::kSidBattery, v, vl));
  // [percent (float64), charging, temperature]
  TEST_ASSERT_EQUAL_HEX8(0x93, v[0]);
  TEST_ASSERT_EQUAL_HEX8(0xCB, v[1]);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xC0, v[10], "unknown charge state must be nil");

  s.chargeKnown = true; s.charging = true;
  const size_t n2 = Telemetry::build(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n2 > 0);
  TEST_ASSERT_TRUE(sensor(buf, n2, Telemetry::kSidBattery, v, vl));
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xC3, v[10], "a known charge state is a boolean");
}

// Half a document is not a smaller reading, it is a parse error at the far
// end — so a buffer that does not fit yields nothing at all.
static void test_a_document_that_does_not_fit_is_not_sent_half_written() {
  const Snapshot s = full();
  uint8_t big[512];
  const size_t whole = Telemetry::build(s, big, sizeof(big));
  TEST_ASSERT_TRUE(whole > 0);
  for (size_t cap = 0; cap < whole; cap++) {
    uint8_t buf[512];
    memset(buf, 0xEE, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, Telemetry::build(s, buf, cap),
                                     "a partial document must not be returned");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xEE, buf[cap], "nothing written past the buffer");
  }
}

// The fields map an answer carries: {FIELD_TELEMETRY: document}.
static void test_the_fields_map_carries_the_document_under_its_field() {
  uint8_t buf[512];
  const size_t n = Telemetry::fields(full(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0x81, buf[0]);                // a map of one
  const uint8_t* doc = nullptr; size_t docLen = 0;
  TEST_ASSERT_TRUE(lxmfField(buf, n, Telemetry::kFieldTelemetry, doc, docLen));
  const uint8_t* v = nullptr; size_t vl = 0;
  TEST_ASSERT_TRUE(sensor(doc, docLen, Telemetry::kSidTime, v, vl));
}

// --- the encoder underneath ---------------------------------------------------

static void test_the_writer_refuses_rather_than_overruns() {
  for (size_t cap = 0; cap < 12; cap++) {
    uint8_t buf[16];
    memset(buf, 0xEE, sizeof(buf));
    MsgPack w(buf, cap);
    w.map(1).uint(0x01).real(1.5);
    if (!w.ok()) TEST_ASSERT_EQUAL_HEX8(0xEE, buf[cap]);
    TEST_ASSERT_TRUE(w.size() <= cap);
  }
}

static void test_the_writer_picks_the_narrowest_form_that_holds_the_value() {
  uint8_t buf[32];
  { MsgPack w(buf, sizeof(buf)); w.uint(5);        TEST_ASSERT_EQUAL_size_t(1, w.size()); }
  { MsgPack w(buf, sizeof(buf)); w.uint(200);      TEST_ASSERT_EQUAL_size_t(2, w.size()); }
  { MsgPack w(buf, sizeof(buf)); w.uint(70000);    TEST_ASSERT_EQUAL_size_t(5, w.size()); }
  { MsgPack w(buf, sizeof(buf)); w.integer(-1);    TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]); }
  { MsgPack w(buf, sizeof(buf)); w.integer(-200);  TEST_ASSERT_EQUAL_HEX8(0xD1, buf[0]); }
  { MsgPack w(buf, sizeof(buf)); w.boolean(true);  TEST_ASSERT_EQUAL_HEX8(0xC3, buf[0]); }
  { MsgPack w(buf, sizeof(buf)); w.nil();          TEST_ASSERT_EQUAL_HEX8(0xC0, buf[0]); }
}

// Everything it writes must be readable by the decoder that reads what
// arrives — the two are the same format seen from opposite sides.
static void test_what_the_writer_emits_the_reader_walks() {
  uint8_t buf[256];
  MsgPack w(buf, sizeof(buf));
  w.array(6).uint(1).integer(-70000).real(2.5).boolean(false).str("hi", 2).bin((const uint8_t*)"\x01\x02", 2);
  TEST_ASSERT_TRUE(w.ok());
  const uint8_t* v = nullptr; size_t vl = 0, next = 0;
  TEST_ASSERT_TRUE(msgpackNext(buf, w.size(), 0, v, vl, next));
  TEST_ASSERT_EQUAL_size_t(w.size(), next);            // the whole array, exactly
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_document_holds_every_sensor_the_node_has);
  RUN_TEST(test_a_board_says_nothing_about_what_it_cannot_measure);
  RUN_TEST(test_information_is_a_string_and_not_bytes);
  RUN_TEST(test_a_position_is_fixed_point_bytes_and_survives_the_trip);
  RUN_TEST(test_a_reading_too_large_for_its_field_is_clamped_not_wrapped);
  RUN_TEST(test_a_charger_a_board_cannot_see_is_not_reported_as_absent);
  RUN_TEST(test_a_document_that_does_not_fit_is_not_sent_half_written);
  RUN_TEST(test_the_fields_map_carries_the_document_under_its_field);
  RUN_TEST(test_the_writer_refuses_rather_than_overruns);
  RUN_TEST(test_the_writer_picks_the_narrowest_form_that_holds_the_value);
  RUN_TEST(test_what_the_writer_emits_the_reader_walks);
  return UNITY_END();
}
