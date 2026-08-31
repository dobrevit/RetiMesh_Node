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
  TEST_ASSERT_EQUAL_HEX8(0x93, out[0]);                 // fixarray of 3
  // bin, not str. LXMF encodes the name with .encode("utf-8") and reads it
  // back with .decode("utf-8"): a str unpacks in Python as str, .decode
  // raises, and the client shows the node with no name at all.
  TEST_ASSERT_EQUAL_HEX8(0xC4, out[1]);
  TEST_ASSERT_EQUAL_HEX8(15, out[2]);
  TEST_ASSERT_EQUAL_STRING_LEN("retimesh-52A7F8", out + 3, 15);
}

// The two elements after the name are not decoration, and getting either
// wrong costs the node every message rather than a nicety.
static void test_no_stamp_cost_is_announced_as_nil_and_never_as_zero() {
  uint8_t out[64];
  const size_t n = lxmfAppData("retimesh-52A7F8", 0, out, sizeof(out));
  // nil. A literal zero reads to a sender as a cost it can satisfy on the
  // first try, so it attaches a stamp — and a stamp lands in the payload
  // after the sender hashed it, which made every message unverifiable.
  TEST_ASSERT_EQUAL_HEX8(0xC0, out[n - 2]);
}

static void test_the_announce_claims_no_functionality_while_there_is_none() {
  uint8_t out[64];
  const size_t n = lxmfAppData("retimesh-52A7F8", 0, out, sizeof(out));
  // An empty list. A shorter announce is read as claiming everything the
  // reader knows of, and the one that matters is compression: the peer would
  // bz2 anything large enough to travel as a resource and the transfer would
  // be refused, which is how long messages went missing.
  TEST_ASSERT_EQUAL_HEX8(0x90, out[n - 1]);
}

static void test_a_stamp_cost_that_is_wanted_is_still_announced() {
  // Nothing asks for one today, but the encoding has to be right on the day
  // something does — a fixint below 128, a uint8 above it.
  uint8_t out[64];
  size_t n = lxmfAppData("n", 12, out, sizeof(out));
  TEST_ASSERT_EQUAL_HEX8(0x0C, out[n - 2]);
  n = lxmfAppData("n", 200, out, sizeof(out));
  TEST_ASSERT_EQUAL_HEX8(0xCC, out[n - 3]);
  TEST_ASSERT_EQUAL_HEX8(200, out[n - 2]);
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

static void test_a_long_name_still_round_trips() {
  char longname[40];
  memset(longname, 'n', 34); longname[34] = '\0';
  uint8_t app[64];
  const size_t n = lxmfAppData(longname, 0, app, sizeof(app));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0xC4, app[1]);                 // bin8 carries any length we emit
  TEST_ASSERT_EQUAL_HEX8(34, app[2]);
  char name[64] = "";
  lxmfName(app, n, name, sizeof(name));
  TEST_ASSERT_EQUAL_STRING(longname, name);
}

// An out-of-range stamp cost is one bad field, not a reason to fall silent:
// returning nothing made the caller skip the whole announce, which would take
// the node's name and address off the network over a number.
static void test_a_stamp_cost_out_of_range_degrades_to_nil() {
  uint8_t out[64];
  const size_t n = lxmfAppData("n", 255, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0xC0, out[n - 2]);
  TEST_ASSERT_EQUAL_HEX8(0x90, out[n - 1]);
}

static void test_a_node_with_no_name_still_announces() {
  uint8_t out[64];
  const size_t n = lxmfAppData(nullptr, 0, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_HEX8(0xC4, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0, out[2]);
}

// Names are UTF-8, and most of the world's are not ASCII. Refusing every byte
// above 0x7E meant a peer called "Café" announced itself and showed up with no
// name at all — the guard was meant to keep control characters out of a
// display and a console, and it threw out the alphabet with them.
static void test_a_name_that_is_not_ascii_still_arrives() {
  struct { const char* in; const char* want; } cases[] = {
    { "Caf\xC3\xA9",                              "Caf\xC3\xA9" },              // Café
    { "\xD0\x9C\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD0\xBD",
      "\xD0\x9C\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD0\xBD" },                     // Мартин
    { "node \xF0\x9F\x93\xA1",                    "node \xF0\x9F\x93\xA1" },    // with a satellite dish
    { "\xE6\x97\xA5\xE6\x9C\xAC",                 "\xE6\x97\xA5\xE6\x9C\xAC" }, // 日本
  };
  for (auto& c : cases) {
    uint8_t app[80];
    const size_t n = lxmfAppData(c.in, 0, app, sizeof(app));
    TEST_ASSERT_TRUE_MESSAGE(n > 0, c.in);
    char name[80] = "";
    TEST_ASSERT_TRUE_MESSAGE(lxmfName(app, n, name, sizeof(name)) > 0, c.in);
    TEST_ASSERT_EQUAL_STRING(c.want, name);
  }
}

// What the guard is actually for. A name is shown on a panel, in a JSON
// document and on a console — none of which should be handed an escape
// sequence or a byte that is not text.
static void test_a_name_that_is_not_text_is_still_refused() {
  const uint8_t bad[][6] = {
    { 'a', 0x1B, '[', '2', 'J', 0 },       // an escape sequence
    { 'a', 0x00, 'b', 0, 0, 0 },           // a NUL in the middle
    { 0xC3, 0x28, 0, 0, 0, 0 },            // a lead byte with no continuation
    { 0xED, 0xA0, 0x80, 0, 0, 0 },         // a UTF-16 surrogate half
    { 0xC0, 0xAF, 0, 0, 0, 0 },            // an overlong slash
    { 0xC2, 0x9B, 0, 0, 0, 0 },            // a C1 control
    { 0xFF, 0xFE, 0, 0, 0, 0 },            // not UTF-8 at all
  };
  const size_t lens[] = { 5, 3, 2, 3, 2, 2, 2 };
  for (size_t k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
    char name[32] = "sentinel";
    TEST_ASSERT_EQUAL_size_t(0, displayableName(bad[k], lens[k], name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("", name);    // and nothing half-copied is left behind
  }
}

// Unicode has control characters of its own, and widening the guard from
// ASCII to UTF-8 let them back in. U+202E reverses the rest of the line it
// lands on, so a name can make a console line or the neighbour list read as
// something else entirely; the zero-width and isolate characters hide or
// reorder text while showing nothing at all.
static void test_a_name_that_reorders_the_line_it_sits_on_is_refused() {
  struct { const char* bytes; size_t len; const char* what; } bad[] = {
    { "gnp.exe\xE2\x80\xAEtxt.", 12, "U+202E right-to-left override" },
    { "node\xE2\x80\x8Bhidden",  13, "U+200B zero-width space" },
    { "a\xE2\x81\xA6b",           5, "U+2066 left-to-right isolate" },
    { "a\xEF\xBB\xBFb",           5, "U+FEFF zero-width no-break space" },
    { "a\xE2\x80\xA8b",           5, "U+2028 line separator" },
  };
  for (auto& c : bad) {
    char name[40] = "sentinel";
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0,
      displayableName((const uint8_t*)c.bytes, c.len, name, sizeof(name)), c.what);
    TEST_ASSERT_EQUAL_STRING("", name);
  }
}

// Message text is held to a different standard than a name: a body carries
// newlines and is kept, not refused. What it must not do is end mid-character.
static void test_message_text_is_cut_between_characters_not_refused() {
  const uint8_t body[] = { 'h','i','\n', 0xC3,0xA9, 0xC3,0xA9 };
  TEST_ASSERT_EQUAL_size_t(7, utf8TrimLen(body, sizeof(body), 99));   // newline kept
  TEST_ASSERT_EQUAL_size_t(5, utf8TrimLen(body, sizeof(body), 6));    // not half of the last é
  TEST_ASSERT_EQUAL_size_t(3, utf8TrimLen(body, sizeof(body), 4));
  TEST_ASSERT_EQUAL_size_t(0, utf8TrimLen(nullptr, 0, 8));
}

// The same text on its way to a serial console, where an escape sequence in a
// message from a stranger would clear the operator's screen.
static void test_message_text_shown_on_the_console_carries_no_escapes() {
  const uint8_t body[] = { 'o','k',0x1B,'[','2','J','\n','d','o','n','e' };
  char shown[32] = "";
  utf8SafeCopy(body, sizeof(body), shown, sizeof(shown));
  TEST_ASSERT_EQUAL_STRING("ok.[2J.done", shown);
  // and it never emits half a character when the buffer runs out
  const uint8_t accents[] = { 0xC3,0xA9, 0xC3,0xA9, 0xC3,0xA9 };
  char small[6] = "";
  const size_t k = utf8SafeCopy(accents, sizeof(accents), small, sizeof(small));
  TEST_ASSERT_EQUAL_size_t(4, k);
  TEST_ASSERT_EQUAL_STRING("\xC3\xA9\xC3\xA9", small);
}

// A map header claiming more pairs than there are bytes must be refused, not
// silently skipped. Counted in size_t on a 32-bit target, count*2 wrapped to
// zero for a map32 claiming 0x80000000 pairs, so the guard passed and the
// walk stepped over the whole map — leaving a signed extent that described a
// different message than the one on the wire.
static void test_a_map_claiming_more_pairs_than_there_are_bytes_is_refused() {
  const uint8_t m32[] = { 0xDF, 0x80, 0x00, 0x00, 0x00, 0xC0 };
  const uint8_t* v = nullptr; size_t vl = 0, next = 0;
  TEST_ASSERT_FALSE(msgpackNext(m32, sizeof(m32), 0, v, vl, next));
  const uint8_t m16[] = { 0xDE, 0xFF, 0xFF, 0xC0 };
  TEST_ASSERT_FALSE(msgpackNext(m16, sizeof(m16), 0, v, vl, next));
}

// Truncation has to land on a character boundary. Half of a multi-byte
// character is not UTF-8, and a JSON encoder or a display driver handed one
// does something worse than showing a shorter name.
static void test_a_name_too_long_for_the_buffer_is_cut_between_characters() {
  // Six two-byte characters: "ééééée"
  const uint8_t name[] = { 0xC3,0xA9, 0xC3,0xA9, 0xC3,0xA9, 0xC3,0xA9, 0xC3,0xA9, 'e' };
  char out[8] = "";                        // room for 7 bytes of text
  const size_t k = displayableName(name, sizeof(name), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(6, k);          // three whole characters, not three and a half
  TEST_ASSERT_EQUAL_STRING("\xC3\xA9\xC3\xA9\xC3\xA9", out);
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

static void test_the_payload_ends_where_its_msgpack_does_not_where_the_packet_does() {
  // The bug this is here to stop coming back: payloadLen was "everything
  // after the signature", so a packet carrying any trailing byte the sender
  // never signed made a genuine message hash differently and look forged.
  // Short messages have no padding, so it passed every test until a real
  // client sent a long one.
  uint8_t buf[256];
  const size_t n = buildMessage(buf, "t", "body");
  memset(buf + n, 0xFF, 24);                   // padding the sender never signed
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 24, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);          // the message, not the padding
  TEST_ASSERT_EQUAL_STRING_LEN("body", m.content, 4);
}

static void test_a_fields_map_is_inside_the_payload_it_measures() {
  // A fields map is what a real client adds and what made the length wrong.
  // It is skipped rather than read — this file has no business interpreting
  // it — but it must be counted.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;                                             // [ts, title, content, fields]
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0x81; *p++ = 0x01; *p++ = 0xA3; *p++ = 'a'; *p++ = 'b'; *p++ = 'c';   // {1:"abc"}
  const size_t n = (size_t)(p - buf);
  memset(buf + n, 0x77, 16);                               // padding after it
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 16, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);          // includes the map, excludes the padding
  TEST_ASSERT_EQUAL_STRING_LEN("hi", m.content, 2);
}

static void test_a_nested_container_is_walked_not_stepped_over() {
  // The bug behind the one above: a container is not one element wide, and
  // skipping its header alone leaves its contents outside the payload — the
  // same fatal result as running past the end, since the bytes hashed are not
  // the bytes signed.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0x81; *p++ = 0x01;                                // {1: [ "ab", 7 ]}
  *p++ = 0x92; *p++ = 0xA2; *p++ = 'a'; *p++ = 'b'; *p++ = 0x07;
  const size_t n = (size_t)(p - buf);
  memset(buf + n, 0x77, 12);
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 12, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);
}

static void test_a_map16_fields_dict_is_walked_like_a_fixmap() {
  // Sixteen entries is where msgpack stops using a fixmap, and the parser
  // refused everything past that header — so a message with a large fields
  // dict was not "a message with fields this node ignores", it was counted as
  // not an LXMF message at all and went unproven. The client shows that as
  // undelivered.
  uint8_t buf[512];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0xDE; *p++ = 0x00; *p++ = 0x11;                   // map16, 17 pairs
  for (uint8_t k = 0; k < 17; k++) { *p++ = k; *p++ = 0xA1; *p++ = 'x'; }
  const size_t n = (size_t)(p - buf);
  memset(buf + n, 0x77, 16);
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 16, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);
  TEST_ASSERT_EQUAL_STRING_LEN("hi", m.content, 2);
}

static void test_an_array16_is_walked_like_a_fixarray() {
  uint8_t buf[512];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0x81; *p++ = 0x01;
  *p++ = 0xDC; *p++ = 0x00; *p++ = 0x14;                   // array16, 20 members
  for (uint8_t k = 0; k < 20; k++) *p++ = k;
  const size_t n = (size_t)(p - buf);
  memset(buf + n, 0x77, 8);
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 8, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);
}

static void test_a_container_claiming_more_members_than_there_are_bytes_is_refused() {
  // The count is the sender's, and a map16 claiming 65535 pairs in a packet
  // of two hundred bytes is either broken or probing. Refusing costs nothing;
  // walking it is 131070 recursive steps before the buffer runs out.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0xDE; *p++ = 0xFF; *p++ = 0xFF;                   // map16, 65535 pairs
  const size_t n = (size_t)(p - buf);
  LxmfMessage m;
  TEST_ASSERT_FALSE(parseLxmf(buf, n, m));
}

static void test_a_str32_field_is_measured_not_refused() {
  // str32 for a short string is legal msgpack and something a generic
  // serialiser does emit. Refusing the header lost the whole message.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  *p++ = 0x81; *p++ = 0x01;
  *p++ = 0xDB; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 3;     // str32 "abc"
  *p++ = 'a'; *p++ = 'b'; *p++ = 'c';
  const size_t n = (size_t)(p - buf);
  memset(buf + n, 0x77, 10);
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, n + 10, m));
  TEST_ASSERT_EQUAL_size_t(n - 96, m.payloadLen);
}

static void test_the_senders_clock_is_read_out_of_the_payload() {
  // The node has no clock of its own on most boards, so the only answer to
  // "when was this written" is the one the sender put in the message. It is
  // a big-endian float64, and reading it little-endian would have produced a
  // date centuries out without ever failing a parse.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x93;
  *p++ = 0xCB;
  const uint8_t ts[8] = {0x41, 0xDA, 0xA5, 0x38, 0x6F, 0x71, 0xF0, 0x22};   // 1767225600.53...
  memcpy(p, ts, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, (size_t)(p - buf), m));
  TEST_ASSERT_TRUE(m.sentAt > 1.7e9 && m.sentAt < 2.0e9);   // a plausible epoch second
}

static void test_a_payload_whose_first_element_is_not_a_timestamp_still_parses() {
  // Nothing here depends on the clock being present, and a message that
  // omitted it used to be a message; refusing one now would be a regression
  // dressed up as validation.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x93;
  *p++ = 0x00;                                    // an integer where the float goes
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA2; *p++ = 'h'; *p++ = 'i';
  LxmfMessage m;
  TEST_ASSERT_TRUE(parseLxmf(buf, (size_t)(p - buf), m));
  TEST_ASSERT_EQUAL_STRING_LEN("hi", m.content, 2);
  TEST_ASSERT_TRUE(m.sentAt == 0.0);
}

static void test_absurd_nesting_is_refused_rather_than_recursed_into() {
  // Depth comes off the wire from strangers, so it is bounded. Refusing is
  // the right answer: LXMF nests two deep and anything claiming more is not a
  // message this node has to understand.
  uint8_t buf[256];
  memset(buf, 0xAA, 16); memset(buf + 16, 0xBB, 16); memset(buf + 32, 0xCC, 64);
  uint8_t* p = buf + 96;
  *p++ = 0x94;
  *p++ = 0xCB; memset(p, 0, 8); p += 8;
  *p++ = 0xA1; *p++ = 't';
  *p++ = 0xA1; *p++ = 'c';
  for (int k = 0; k < 40; k++) *p++ = 0x91;                // an array inside an array, forty deep
  *p++ = 0x00;
  LxmfMessage m;
  TEST_ASSERT_FALSE(parseLxmf(buf, (size_t)(p - buf), m));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_announce_carries_the_name_a_client_will_show);
  RUN_TEST(test_no_stamp_cost_is_announced_as_nil_and_never_as_zero);
  RUN_TEST(test_the_announce_claims_no_functionality_while_there_is_none);
  RUN_TEST(test_a_stamp_cost_that_is_wanted_is_still_announced);
  RUN_TEST(test_what_it_emits_is_what_it_reads_back);
  RUN_TEST(test_a_long_name_still_round_trips);
  RUN_TEST(test_a_stamp_cost_out_of_range_degrades_to_nil);
  RUN_TEST(test_a_node_with_no_name_still_announces);
  RUN_TEST(test_a_name_that_is_not_ascii_still_arrives);
  RUN_TEST(test_a_name_that_is_not_text_is_still_refused);
  RUN_TEST(test_a_name_that_reorders_the_line_it_sits_on_is_refused);
  RUN_TEST(test_a_name_too_long_for_the_buffer_is_cut_between_characters);
  RUN_TEST(test_a_name_that_will_not_fit_is_refused_not_truncated);
  RUN_TEST(test_message_text_is_cut_between_characters_not_refused);
  RUN_TEST(test_message_text_shown_on_the_console_carries_no_escapes);
  RUN_TEST(test_a_map_claiming_more_pairs_than_there_are_bytes_is_refused);
  RUN_TEST(test_a_well_formed_message_yields_its_text);
  RUN_TEST(test_an_empty_title_is_allowed);
  RUN_TEST(test_a_truncated_envelope_is_refused);
  RUN_TEST(test_a_payload_that_is_not_an_array_is_refused);
  RUN_TEST(test_a_length_that_runs_off_the_end_is_refused);
  RUN_TEST(test_a_message_of_nothing_but_an_envelope_is_refused);
  RUN_TEST(test_the_payload_ends_where_its_msgpack_does_not_where_the_packet_does);
  RUN_TEST(test_a_fields_map_is_inside_the_payload_it_measures);
  RUN_TEST(test_a_nested_container_is_walked_not_stepped_over);
  RUN_TEST(test_a_map16_fields_dict_is_walked_like_a_fixmap);
  RUN_TEST(test_an_array16_is_walked_like_a_fixarray);
  RUN_TEST(test_a_container_claiming_more_members_than_there_are_bytes_is_refused);
  RUN_TEST(test_a_str32_field_is_measured_not_refused);
  RUN_TEST(test_the_senders_clock_is_read_out_of_the_payload);
  RUN_TEST(test_a_payload_whose_first_element_is_not_a_timestamp_still_parses);
  RUN_TEST(test_absurd_nesting_is_refused_rather_than_recursed_into);
  return UNITY_END();
}
