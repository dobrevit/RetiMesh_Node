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
#include "LxmfCommands.h"
#include "vectors.h"

using namespace Rns;

// What RnsTransport::handleLxmfMessage hashes — asked of the same function it
// asks, rather than rebuilt here. Rebuilding it would have made this test
// unable to fail when the node's copy changed, which is the one thing it is
// for.
static std::vector<uint8_t> hashedPart(const LxmfMessage& m) {
  std::vector<uint8_t> hp;
  lxmfSignedSpans(m, [&](const uint8_t* p, size_t n) { hp.insert(hp.end(), p, p + n); });
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

// --- the announce direction --------------------------------------------------
//
// The half that was missing, and the gap that let this node announce its name
// as a msgpack str for a release: LXMF writes the name with .encode("utf-8"),
// so it goes out as bin and comes back through .decode("utf-8"). A str
// announce unpacks in Python as str, .decode raises, and the client shows the
// node with no name. Our own reader takes either, so round-tripping our
// encoder against our decoder agreed with itself and proved nothing.

static void test_we_read_the_name_out_of_a_real_lxmf_announce() {
  for (const auto& v : kLxmfAnnounceVectors) {
    char name[80] = "";
    const size_t k = lxmfName(v.appData, v.appDataLen, name, sizeof(name));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(v.name), k, v.tag);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(v.name, name, v.tag);
  }
}

// And the same bytes back out. Not a round-trip against ourselves: the
// expected bytes are the ones LXMRouter produced, so an encoding this node
// invents fails here.
static void test_what_we_announce_is_what_lxmf_would_have_announced() {
  for (const auto& v : kLxmfAnnounceVectors) {
    if (v.name[0] == '\0') continue;           // LXMF writes nil for no name; the node always has one
    uint8_t ours[96];
    const size_t n = lxmfAppData(v.name, 0, ours, sizeof(ours));
    TEST_ASSERT_TRUE_MESSAGE(n > 0, v.tag);
    // The name element must match LXMF's byte for byte — header included.
    // Everything after it is this node's own policy (no stamp, no
    // compression) and deliberately differs from the vector.
    const size_t nameBytes = 2 + strlen(v.name);           // 0xC4, len, name
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(v.appData, ours, 1 + nameBytes, v.tag);
  }
}

// The reader has to keep taking a str announce, whatever we emit: older LXMF
// clients are out there and a name we can read is better than none.
static void test_a_str_announce_from_an_older_client_is_still_read() {
  const char* name = "older-client";
  const size_t n = strlen(name);
  uint8_t app[64];
  app[0] = 0x93; app[1] = (uint8_t)(0xA0 | n);
  memcpy(app + 2, name, n);
  app[2 + n] = 0xC0; app[3 + n] = 0x90;
  char out[40] = "";
  TEST_ASSERT_EQUAL_size_t(n, lxmfName(app, 4 + n, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(name, out);
}

// --- commands ----------------------------------------------------------------
//
// What a person in a field actually asks a node, sent by Sideband's own
// buttons. These vectors are messages the library built, so the field walk is
// held to what a client sends rather than to what we imagined it sends.

static const LxmfCommandVector& commandVector(const char* tag) {
  for (const auto& v : kLxmfCommandVectors) if (strcmp(v.tag, tag) == 0) return v;
  TEST_FAIL_MESSAGE("no such command vector");
  return kLxmfCommandVectors[0];
}

// Parse a command vector down to the commands it carries.
static size_t commandsOf(const LxmfCommandVector& v, LxmfCommand* out, size_t max) {
  LxmfMessage m;
  TEST_ASSERT_TRUE_MESSAGE(parseLxmf(v.wire, v.wireLen, m), v.tag);
  TEST_ASSERT_TRUE_MESSAGE(m.fieldsCount >= 1, v.tag);
  const uint8_t* val = nullptr; size_t valLen = 0;
  if (!lxmfField(m.fields, m.fieldsLen, kFieldCommands, val, valLen)) return 0;
  return lxmfCommands(val, valLen, out, max);
}

static void test_a_ping_from_a_real_client_is_seen_as_one() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_ping"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(kCommandPing, c[0].id);
}

static void test_an_echo_carries_the_text_to_send_back() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_echo"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(kCommandEcho, c[0].id);
  TEST_ASSERT_EQUAL_size_t(13, c[0].textLen);
  TEST_ASSERT_EQUAL_MEMORY("are you there", c[0].text, 13);
}

static void test_a_signal_report_is_seen_as_one() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_signal"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(kCommandSignal, c[0].id);
}

static void test_several_commands_arrive_in_the_order_they_were_sent() {
  LxmfCommand c[8];
  TEST_ASSERT_EQUAL_size_t(3, commandsOf(commandVector("cmd_all"), c, 8));
  TEST_ASSERT_EQUAL_UINT32(kCommandEcho, c[0].id);
  TEST_ASSERT_EQUAL_MEMORY("hello", c[0].text, 5);
  TEST_ASSERT_EQUAL_UINT32(kCommandSignal, c[1].id);
  TEST_ASSERT_EQUAL_UINT32(kCommandPing, c[2].id);
}

// A client may send commands this node has never heard of, and it must not
// cost the node the ones it does understand.
static void test_a_command_we_do_not_know_does_not_hide_the_one_we_do() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(2, commandsOf(commandVector("cmd_unknown_first"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(0x7E, c[0].id);
  TEST_ASSERT_EQUAL_UINT32(kCommandPing, c[1].id);
}

// A telemetry request's argument is a list, not a bare true. This node does
// not answer them yet; what matters is that one does not derail the walk.
static void test_a_telemetry_request_is_read_without_being_answered() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(2, commandsOf(commandVector("cmd_telemetry"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(kCommandTelemetry, c[0].id);
  TEST_ASSERT_NULL(c[0].text);                   // a list, so there is no text to read
  TEST_ASSERT_EQUAL_UINT32(kCommandPing, c[1].id);
}

// The commands field is one of several, and the walk has to keep its place
// past values it does not read to find it.
static void test_commands_are_found_among_other_fields() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_among_fields"), c, 4));
  TEST_ASSERT_EQUAL_UINT32(kCommandPing, c[0].id);
}

// An ordinary message has no commands in it, and asking must not find any.
static void test_a_message_with_no_commands_yields_none() {
  for (const auto& v : kLxmfVectors) {
    LxmfMessage m;
    std::vector<uint8_t> b = asReceived(v);
    TEST_ASSERT_TRUE_MESSAGE(parseLxmf(b.data(), b.size(), m), v.tag);
    const uint8_t* val = nullptr; size_t valLen = 0;
    TEST_ASSERT_FALSE_MESSAGE(lxmfField(m.fields, m.fieldsLen, kFieldCommands, val, valLen), v.tag);
  }
}

// Nothing here may read past the buffer, whatever a stranger sends.
static void test_no_truncated_command_message_reads_past_its_buffer() {
  for (const auto& v : kLxmfCommandVectors) {
    for (size_t cut = 0; cut < v.wireLen; cut++) {
      LxmfMessage m;
      if (!parseLxmf(v.wire, cut, m)) continue;
      const uint8_t* val = nullptr; size_t valLen = 0;
      if (!lxmfField(m.fields, m.fieldsLen, kFieldCommands, val, valLen)) continue;
      LxmfCommand c[8];
      const size_t k = lxmfCommands(val, valLen, c, 8);
      for (size_t j = 0; j < k; j++)
        if (c[j].text)
          TEST_ASSERT_TRUE_MESSAGE(c[j].text + c[j].textLen <= v.wire + cut, v.tag);
    }
  }
}

// The whole way through, on bytes the library built: a real client's message
// in, the text that goes back out. The two halves are tested apart elsewhere;
// this is the one that fails if they stop meeting in the middle.
static void test_a_real_ping_produces_the_answer_a_client_expects() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_ping"), c, 4));
  char out[Commands::kReplyMax] = "";
  Commands::Signal s; s.rssi = -97.0f; s.snr = 6.25f;
  TEST_ASSERT_TRUE(Commands::reply(c[0], s, out, sizeof(out)) > 0);
  TEST_ASSERT_EQUAL_STRING("Ping reply", out);
}

static void test_a_real_echo_comes_back_with_the_senders_own_words() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_echo"), c, 4));
  char out[Commands::kReplyMax] = "";
  Commands::reply(c[0], {}, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Echo reply: are you there", out);
}

static void test_a_real_signal_request_reports_what_the_radio_measured() {
  LxmfCommand c[4];
  TEST_ASSERT_EQUAL_size_t(1, commandsOf(commandVector("cmd_signal"), c, 4));
  char out[Commands::kReplyMax] = "";
  Commands::Signal s; s.rssi = -104.0f; s.snr = 8.75f;
  Commands::reply(c[0], s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("RSSI: -104 dBm\nSNR: 8.8 dB", out);
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
  RUN_TEST(test_we_read_the_name_out_of_a_real_lxmf_announce);
  RUN_TEST(test_what_we_announce_is_what_lxmf_would_have_announced);
  RUN_TEST(test_a_str_announce_from_an_older_client_is_still_read);
  RUN_TEST(test_a_ping_from_a_real_client_is_seen_as_one);
  RUN_TEST(test_an_echo_carries_the_text_to_send_back);
  RUN_TEST(test_a_signal_report_is_seen_as_one);
  RUN_TEST(test_several_commands_arrive_in_the_order_they_were_sent);
  RUN_TEST(test_a_command_we_do_not_know_does_not_hide_the_one_we_do);
  RUN_TEST(test_a_telemetry_request_is_read_without_being_answered);
  RUN_TEST(test_commands_are_found_among_other_fields);
  RUN_TEST(test_a_message_with_no_commands_yields_none);
  RUN_TEST(test_no_truncated_command_message_reads_past_its_buffer);
  RUN_TEST(test_a_real_ping_produces_the_answer_a_client_expects);
  RUN_TEST(test_a_real_echo_comes_back_with_the_senders_own_words);
  RUN_TEST(test_a_real_signal_request_reports_what_the_radio_measured);
  return UNITY_END();
}
