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
  char accents[200];
  for (size_t i = 0; i + 1 < sizeof(accents) - 1; i += 2) { accents[i] = (char)0xC3; accents[i+1] = (char)0xA9; }
  accents[sizeof(accents) - 1] = '\0';
  const size_t k = Commands::reply(cmd(kCommandEcho, accents), {}, out, sizeof(out));
  const size_t body = k - strlen("Echo reply: ");
  TEST_ASSERT_EQUAL_size_t(0, body % 2);        // whole characters only
}

// A body that is not text is not reflected back over the radio at all.
static void test_an_echo_of_something_that_is_not_text_carries_none_of_it() {
  char out[Commands::kReplyMax] = "";
  LxmfCommand c{};
  c.id = kCommandEcho;
  c.text = (const uint8_t*)"\xFF\xFE\x01";
  c.textLen = 3;
  Commands::reply(c, {}, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Echo reply: ", out);
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

// A telemetry request is a real question this node cannot answer yet. Saying
// nothing leaves the asker's client showing it unanswered, which is true; a
// reply saying "no" would put a confusing message in their conversation.
static void test_a_command_this_node_cannot_answer_gets_no_reply() {
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
  RUN_TEST(test_an_echo_of_something_that_is_not_text_carries_none_of_it);
  RUN_TEST(test_a_signal_report_reads_like_the_one_a_phone_sends);
  RUN_TEST(test_a_figure_the_node_does_not_have_is_left_out);
  RUN_TEST(test_a_message_with_no_reading_at_all_says_so);
  RUN_TEST(test_a_command_this_node_cannot_answer_gets_no_reply);
  RUN_TEST(test_no_reply_overruns_a_small_buffer);
  return UNITY_END();
}
