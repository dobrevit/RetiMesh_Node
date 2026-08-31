// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The parser against messages the reference library actually produced.
//
// test_lxmf/ builds its buffers by hand, which makes it a good test of what
// the parser refuses and a poor one of what clients send: every vector there
// is the shape we believed in when we wrote it. Two faults lived comfortably
// underneath a green suite because of that — a message carrying a stamp, and
// a message sent as a single packet — and both were found on the bench rather
// than here.
//
// These vectors come from LXMF itself (tools/lxmf_vectors.py). Each one
// carries the bytes as they arrive at the delivery destination and the
// hashed_part the library computed over them. Reproducing those bytes is the
// whole of interoperability: the signature is taken over hashed_part and its
// own hash, so a parser that agrees on them verifies the sender, and one that
// disagrees calls an honest sender a forger. Checking the bytes rather than
// the signature is what lets this run natively, with no crypto linked in.

#include <unity.h>
#include <string.h>
#include <vector>
#include "LxmfFormat.h"
#include "vectors.h"

using namespace Rns;

// What RnsTransport::handleLxmfMessage hashes, kept in the same order it
// builds it. If this and the library disagree by a byte, nothing verifies.
static std::vector<uint8_t> hashedPart(const LxmfMessage& m) {
  std::vector<uint8_t> hp;
  hp.insert(hp.end(), m.destHash, m.destHash + 16);
  hp.insert(hp.end(), m.sourceHash, m.sourceHash + 16);
  hp.push_back(m.signedHeader);
  hp.insert(hp.end(), m.signedBody, m.signedBody + m.signedBodyLen);
  return hp;
}

// A single packet arrives without its destination hash — the sender strips it
// and the receiving router puts it back, which is what onLxmfPacket does.
static std::vector<uint8_t> asReceived(const LxmfVector& v) {
  std::vector<uint8_t> b;
  if (v.dest) b.insert(b.end(), v.dest, v.dest + 16);
  b.insert(b.end(), v.wire, v.wire + v.wireLen);
  return b;
}

static void test_every_message_a_real_client_sends_parses() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
  }
}

// The assertion this file exists for. A stamped message is the case that was
// wrong: LXMF appends the stamp after hashing and drops it again before
// checking, so hashing the payload as it arrived disagrees with the sender.
static void test_what_we_hash_is_what_the_library_signed() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    const std::vector<uint8_t> hp = hashedPart(m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(v.hashedLen, hp.size(), v.tag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(v.hashed, hp.data(), v.hashedLen, v.tag);
  }
}

static void test_the_text_survives_the_journey() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(v.content), m.contentLen, v.tag);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(v.content, m.content, m.contentLen, v.tag);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(v.title), m.titleLen, v.tag);
    if (m.titleLen) TEST_ASSERT_EQUAL_MEMORY_MESSAGE(v.title, m.title, m.titleLen, v.tag);
  }
}

// Whether a stamp was there is not something the parser should have to guess
// at, and it is what an operator needs to read a mismatch: a stamped message
// only arrives when this node announced a cost it did not mean to.
static void test_a_stamp_is_seen_and_excluded() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    TEST_ASSERT_EQUAL_MESSAGE(v.stamped, m.stamped, v.tag);
    // A stamp lives past the end of what was signed, so the payload the
    // message occupies is longer than the part the sender hashed.
    if (v.stamped) TEST_ASSERT_TRUE_MESSAGE(m.payloadLen > m.signedBodyLen + 1, v.tag);
    else           TEST_ASSERT_EQUAL_size_t_MESSAGE(m.payloadLen, m.signedBodyLen + 1, v.tag);
  }
}

// Every LXMF message carries a fields map, even an empty one, and a message
// that carried an attachment carries a large one. Reporting its extent is
// what lets the inbox say "this one had something" instead of showing a body
// that looks blank.
static void test_the_fields_map_is_located_not_read() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    TEST_ASSERT_NOT_NULL_MESSAGE(m.fields, v.tag);
    TEST_ASSERT_TRUE_MESSAGE(m.fieldsLen >= 1, v.tag);
    // It sits inside the payload, never past it.
    TEST_ASSERT_TRUE_MESSAGE(m.fields >= m.payload, v.tag);
    TEST_ASSERT_TRUE_MESSAGE((size_t)(m.fields - m.payload) + m.fieldsLen <= m.payloadLen, v.tag);
  }
  // The one with a photo in it has a fields map far larger than an empty map.
  for (const auto& v : kLxmfVectors) {
    if (strcmp(v.tag, "attachment") != 0) continue;
    const std::vector<uint8_t> b = asReceived(v);
    LxmfMessage m;
    TEST_ASSERT_TRUE(parseLxmf(b.data(), b.size(), m));
    TEST_ASSERT_TRUE(m.fieldsLen > 64);
  }
}

// Trailing bytes the sender never signed must not reach the hash. A packet
// can be padded, and hashing the padding makes a genuine message look forged.
static void test_padding_after_a_real_message_is_ignored() {
  for (const auto& v : kLxmfVectors) {
    std::vector<uint8_t> b = asReceived(v);
    const size_t n = b.size();
    b.resize(n + 24, 0x00);
    LxmfMessage m;
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    const std::vector<uint8_t> hp = hashedPart(m);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(v.hashed, hp.data(), v.hashedLen, v.tag);
  }
}

// Truncation at every length, against messages of real shapes rather than
// built ones. Nothing may be read past the buffer, and nothing short may be
// accepted as whole.
static void test_no_truncation_of_a_real_message_is_accepted_as_whole() {
  for (const auto& v : kLxmfVectors) {
    const std::vector<uint8_t> b = asReceived(v);
    for (size_t cut = 0; cut < b.size(); cut++) {
      LxmfMessage m;
      if (parseLxmf(b.data(), cut, m)) {
        // Accepting a prefix is allowed only where the message genuinely ends
        // before the padding does — never past the bytes handed over.
        TEST_ASSERT_TRUE_MESSAGE(m.payloadLen + 96 <= cut, v.tag);
      }
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_message_a_real_client_sends_parses);
  RUN_TEST(test_what_we_hash_is_what_the_library_signed);
  RUN_TEST(test_the_text_survives_the_journey);
  RUN_TEST(test_a_stamp_is_seen_and_excluded);
  RUN_TEST(test_the_fields_map_is_located_not_read);
  RUN_TEST(test_padding_after_a_real_message_is_ignored);
  RUN_TEST(test_no_truncation_of_a_real_message_is_accepted_as_whole);
  return UNITY_END();
}
