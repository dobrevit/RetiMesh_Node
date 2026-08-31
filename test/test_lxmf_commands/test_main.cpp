// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// What the node says back when a stranger asks it something.
//
// Every string here leaves the node over its radio because somebody it has
// never met sent a packet, so the two things worth holding it to are that the
// wording is what a client expects to read, and that nothing a sender puts in
// a message can make the reply longer, malformed, or something other than
// text.

#include <unity.h>
#include <string.h>
#include "LxmfCommands.h"

using namespace Rns;
using Rns::Commands::Signal;

static LxmfCommand cmd(uint32_t id, const char* text = nullptr) {
  LxmfCommand c{};
  c.id = id;
  c.text = (const uint8_t*)text;
  c.textLen = text ? strlen(text) : 0;
  return c;
}

static void test_a_ping_is_answered_the_way_sideband_answers_one() {
  char out[Commands::kReplyMax] = "";
  const size_t n = Commands::reply(cmd(kCommandPing), {}, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(10, n);
  TEST_ASSERT_EQUAL_STRING("Ping reply", out);
}

static void test_an_echo_comes_back_with_what_was_sent() {
  char out[Commands::kReplyMax] = "";
  Commands::reply(cmd(kCommandEcho, "are you there"), {}, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Echo reply: are you there", out);
}

// The echoed text is a stranger's, and it is about to be transmitted. It may
// not run off the end of the buffer, and it may not end mid-character.
static void test_an_echo_is_bounded_and_cut_between_characters() {
  char out[Commands::kReplyMax] = "";
  char huge[400];
  memset(huge, 'x', sizeof(huge) - 1); huge[sizeof(huge) - 1] = '\0';
  const size_t n = Commands::reply(cmd(kCommandEcho, huge), {}, out, sizeof(out));
  TEST_ASSERT_TRUE(n < Commands::kReplyMax);
  TEST_ASSERT_EQUAL_size_t(n, strlen(out));

  // Two-byte characters, cut so that the limit falls inside one.
  // Odd-sized on purpose: pairs fill every byte up to the terminator, so no
  // byte is left unwritten for strlen to read. An even buffer cannot do that —
  // which is what the previous two attempts at this line got wrong.
  char accents[199];
  for (size_t i = 0; i + 1 < sizeof(accents) - 1; i += 2) { accents[i] = (char)0xC3; accents[i+1] = (char)0xA9; }
  accents[sizeof(accents) - 1] = '\0';
  const size_t k = Commands::reply(cmd(kCommandEcho, accents), {}, out, sizeof(out));
  const size_t body = k - strlen("Echo reply: ");
  TEST_ASSERT_EQUAL_size_t(0, body % 2);        // whole characters only
}

// The echoed bytes come from a stranger and leave over this node's signature,
// so what is reflected is text and only text. Anything else is shown as a dot
// rather than dropped, so the reply says something arrived without repeating
// it verbatim into whatever is reading.
static void test_an_echo_reflects_text_and_nothing_else() {
  struct { const char* in; size_t len; const char* want; const char* what; } cases[] = {
    { "\xFF\xFE\x01", 3, "Echo reply: ...", "bytes that are not UTF-8" },
    { "\x1b[2J", 4, "Echo reply: .[2J", "a screen-clearing escape sequence" },
    // The literal is split because a C hex escape is maximal-munch: "a\x07b"
    // is 'a' followed by 0x7B, not a bell followed by 'b'.
    { "a\x07" "b", 3, "Echo reply: a.b", "a bell" },
    { "a\xE2\x80\xAE" "b", 5, "Echo reply: a...b", "U+202E, which reverses the line" },
    { "ok\nnext", 7, "Echo reply: ok.next", "a newline, since a reply is one line" },
  };
  for (auto& c : cases) {
    char out[Commands::kReplyMax] = "";
    LxmfCommand k{};
    k.id = kCommandEcho;
    k.text = (const uint8_t*)c.in;
    k.textLen = c.len;
    Commands::reply(k, {}, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.want, out, c.what);
  }
}

// Every branch that returns 0 leaves an empty string behind. The signal branch
// used to measure by writing into the caller's buffer, so a buffer too small
// for the fallback was left holding half a line beside a return of nothing.
static void test_returning_nothing_leaves_nothing_behind() {
  Signal s; s.rssi = -104.0f; s.snr = 8.75f; s.q = 100.0f;
  for (size_t cap = 1; cap < 28; cap++) {
    char buf[64];
    memset(buf, '#', sizeof(buf));
    const size_t n = Commands::reply(cmd(kCommandSignal), s, buf, cap);
    if (n == 0) TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
  }
}

static void test_a_signal_report_reads_like_the_one_a_phone_sends() {
  char out[Commands::kReplyMax] = "";
  Signal s; s.rssi = -104.0f; s.snr = 8.75f; s.q = 42.0f;
  Commands::reply(cmd(kCommandSignal), s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Link Quality: 42%\nRSSI: -104 dBm\nSNR: 8.8 dB", out);
}

// A figure the node does not have is left out, not filled in with a zero: a
// message that arrived over Wi-Fi has no RSSI, and saying "0 dBm" would be a
// reading rather than an absence.
static void test_a_figure_the_node_does_not_have_is_left_out() {
  char out[Commands::kReplyMax] = "";
  Signal s; s.rssi = -91.0f;                     // snr and q stay NaN
  Commands::reply(cmd(kCommandSignal), s, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("RSSI: -91 dBm", out);
}

static void test_a_message_with_no_reading_at_all_says_so() {
  char out[Commands::kReplyMax] = "";
  Commands::reply(cmd(kCommandSignal), {}, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("No reception info available", out);
}

// Which commands are answered, and how. One predicate, because the transport
// asks it too — it decides whether to attach the node's readings, and the two
// briefly disagreed about telemetry.
static void test_the_node_says_which_commands_it_answers() {
  TEST_ASSERT_TRUE(Commands::answers(kCommandPing) == Commands::Answer::Text);
  TEST_ASSERT_TRUE(Commands::answers(kCommandEcho) == Commands::Answer::Text);
  TEST_ASSERT_TRUE(Commands::answers(kCommandSignal) == Commands::Answer::Text);
  TEST_ASSERT_TRUE(Commands::answers(kCommandTelemetry) == Commands::Answer::Telemetry);
  TEST_ASSERT_TRUE(Commands::answers(0x7E) == Commands::Answer::None);
}

// A telemetry request is answered with readings, not a sentence — a sentence
// beside them would appear in somebody's conversation as a message a person
// sent. reply() therefore writes nothing for it, and so it does for a command
// this node has never heard of.
static void test_no_text_is_written_for_what_is_not_a_text_answer() {
  char out[Commands::kReplyMax] = "sentinel";
  TEST_ASSERT_EQUAL_size_t(0, Commands::reply(cmd(kCommandTelemetry), {}, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_size_t(0, Commands::reply(cmd(0x7E), {}, out, sizeof(out)));
}

// Nothing may be written past the buffer, whatever it is asked for.
static void test_no_reply_overruns_a_small_buffer() {
  Signal s; s.rssi = -104.0f; s.snr = 8.75f; s.q = 100.0f;
  const uint32_t ids[] = { kCommandPing, kCommandEcho, kCommandSignal };
  for (size_t cap = 1; cap < 48; cap++) {
    for (uint32_t id : ids) {
      char buf[64];
      memset(buf, '#', sizeof(buf));
      const size_t n = Commands::reply(cmd(id, "some text here"), s, buf, cap);
      TEST_ASSERT_TRUE(n < cap);
      TEST_ASSERT_EQUAL_CHAR('#', buf[cap]);     // nothing written past what it was given
      if (n) TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
    }
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_ping_is_answered_the_way_sideband_answers_one);
  RUN_TEST(test_an_echo_comes_back_with_what_was_sent);
  RUN_TEST(test_an_echo_is_bounded_and_cut_between_characters);
  RUN_TEST(test_an_echo_reflects_text_and_nothing_else);
  RUN_TEST(test_returning_nothing_leaves_nothing_behind);
  RUN_TEST(test_a_signal_report_reads_like_the_one_a_phone_sends);
  RUN_TEST(test_a_figure_the_node_does_not_have_is_left_out);
  RUN_TEST(test_a_message_with_no_reading_at_all_says_so);
  RUN_TEST(test_the_node_says_which_commands_it_answers);
  RUN_TEST(test_no_text_is_written_for_what_is_not_a_text_answer);
  RUN_TEST(test_no_reply_overruns_a_small_buffer);
  return UNITY_END();
}
