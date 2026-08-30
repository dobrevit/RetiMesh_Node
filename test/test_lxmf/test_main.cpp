// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The LXMF wire format, both directions. These bytes arrive over the air from
// anyone in earshot, so what matters most here is what the parser refuses: a
// truncated envelope, a payload that is not the array it claims, a length
// that runs off the end. A parser that reads past its buffer on a malformed
// message is a parser an attacker writes the message for.

#include <unity.h>
#include <string.h>
#include "LxmfFormat.h"

using namespace Rns;

// --- what the node emits -----------------------------------------------------
static void test_the_announce_carries_the_name_a_client_will_show() {
  uint8_t out[64];
  const size_t n = lxmfAppData("retimesh-52A7F8", 0, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0x92, out[0]);                 // fixarray of 2
  TEST_ASSERT_EQUAL_HEX8(0xA0 | 15, out[1]);            // fixstr, 15 bytes
  TEST_ASSERT_EQUAL_STRING_LEN("retimesh-52A7F8", out + 2, 15);
  TEST_ASSERT_EQUAL_HEX8(0x00, out[n - 1]);             // stamp cost
}

static void test_what_it_emits_is_what_it_reads_back() {
  // The two directions are one format, and this is the test that keeps them
  // one: displayName() is what the node uses on every announce it hears.
  uint8_t app[64];
  const size_t n = lxmfAppData("retimesh-EAD2C8", 0, app, sizeof(app));
  char name[40] = "";
  TEST_ASSERT_TRUE(lxmfName(app, n, name, sizeof(name)) > 0);
  TEST_ASSERT_EQUAL_STRING("retimesh-EAD2C8", name);
}

static void test_a_long_name_uses_str8_and_still_round_trips() {
  char longname[40];
  memset(longname, 'n', 34); longname[34] = '\0';
  uint8_t app[64];
  const size_t n = lxmfAppData(longname, 0, app, sizeof(app));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0xD9, app[1]);                 // str8, past fixstr's 31
  char name[64] = "";
  lxmfName(app, n, name, sizeof(name));
  TEST_ASSERT_EQUAL_STRING(longname, name);
}

static void test_a_name_that_will_not_fit_is_refused_not_truncated() {
  uint8_t tiny[8];
  TEST_ASSERT_EQUAL_size_t(0, lxmfAppData("retimesh-52A7F8", 0, tiny, sizeof(tiny)));
}

// --- what the node receives --------------------------------------------------
// dest(16) | source(16) | signature(64) | msgpack [ts, title, content, fields]
static size_t buildMessage(uint8_t* out, const char* title, const char* content) {
  memset(out, 0xAA, 16);                                // destination
  memset(out + 16, 0xBB, 16);                           // source
  memset(out + 32, 0xCC, 64);                           // signature
  uint8_t* p = out + 96;
  *p++ = 0x94;                                          // fixarray of 4
  *p++ = 0xCB; memset(p, 0, 8); p += 8;                 // timestamp, float64
  const size_t tl = strlen(title), cl = strlen(content);
  *p++ = (uint8_t)(0xA0 | tl); memcpy(p, title, tl); p += tl;
  *p++ = (uint8_t)(0xA0 | cl); memcpy(p, content, cl); p += cl;
  *p++ = 0x80;                                          // fields: empty fixmap
  return (size_t)(p - out);
}

static void test_a_well_formed_message_yields_its_text() {
  uint8_t buf[256];
  const size_t n = buildMessage(buf, "hi", "STATUS please");
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n, m));
  TEST_ASSERT_EQUAL_HEX8(0xBB, m.sourceHash[0]);
  TEST_ASSERT_EQUAL_size_t(2, m.titleLen);
  TEST_ASSERT_EQUAL_STRING_LEN("hi", m.title, 2);
  TEST_ASSERT_EQUAL_size_t(13, m.contentLen);
  TEST_ASSERT_EQUAL_STRING_LEN("STATUS please", m.content, 13);
  // The signature covers the two hashes and the payload, so the payload the
  // caller verifies has to be exactly what followed the signature.
  TEST_ASSERT_EQUAL_PTR(buf + 96, m.payload);
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);
}

static void test_an_empty_title_is_allowed() {
  uint8_t buf[256];
  const size_t n = buildMessage(buf, "", "no subject");
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n, m));
  TEST_ASSERT_EQUAL_size_t(0, m.titleLen);
  TEST_ASSERT_EQUAL_STRING_LEN("no subject", m.content, 10);
}

static void test_a_truncated_envelope_is_refused() {
  uint8_t buf[256];
  const size_t n = buildMessage(buf, "t", "c");
  LxmfMessage m;
  for (size_t cut = 0; cut < 97; cut++)
    TEST_ASSERT_FALSE_MESSAGE(parseLxmf(buf, cut, m), "a message shorter than its envelope");
  TEST_ASSERT_TRUE(parseLxmf(buf, n, m));
}

static void test_a_payload_that_is_not_an_array_is_refused() {
  uint8_t buf[256];
  size_t n = buildMessage(buf, "t", "c");
  LxmfMessage m;
  buf[96] = 0xC0;                                       // nil where the array was
  TEST_ASSERT_FALSE(parseLxmf(buf, n, m));
  buf[96] = 0x92;                                       // an array, but of two
  TEST_ASSERT_FALSE(parseLxmf(buf, n, m));
}

static void test_a_length_that_runs_off_the_end_is_refused() {
  // The case that matters: a string header claiming more than the buffer
  // holds. Reading it would walk off the end of a packet somebody else wrote.
  uint8_t buf[256];
  size_t n = buildMessage(buf, "t", "c");
  buf[106] = 0xA0 | 31;                                 // title claims 31 bytes
  LxmfMessage m;
  TEST_ASSERT_FALSE(parseLxmf(buf, n, m));
  n = buildMessage(buf, "t", "c");
  buf[106] = 0xD9; buf[107] = 0xFF;                     // str8 claiming 255
  TEST_ASSERT_FALSE(parseLxmf(buf, n, m));
}

static void test_a_message_of_nothing_but_an_envelope_is_refused() {
  uint8_t buf[128];
  memset(buf, 0, sizeof(buf));
  LxmfMessage m;
  TEST_ASSERT_FALSE(parseLxmf(buf, 96, m));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_announce_carries_the_name_a_client_will_show);
  RUN_TEST(test_what_it_emits_is_what_it_reads_back);
  RUN_TEST(test_a_long_name_uses_str8_and_still_round_trips);
  RUN_TEST(test_a_name_that_will_not_fit_is_refused_not_truncated);
  RUN_TEST(test_a_well_formed_message_yields_its_text);
  RUN_TEST(test_an_empty_title_is_allowed);
  RUN_TEST(test_a_truncated_envelope_is_refused);
  RUN_TEST(test_a_payload_that_is_not_an_array_is_refused);
  RUN_TEST(test_a_length_that_runs_off_the_end_is_refused);
  RUN_TEST(test_a_message_of_nothing_but_an_envelope_is_refused);
  return UNITY_END();
}
