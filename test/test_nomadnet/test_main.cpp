// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The node's own page. It is generated on the RNS task because a stranger
// asked for it, so the two things worth holding it to are that it never
// overruns the buffer it was given, and that it says nothing the board cannot
// actually measure — the same doctrine as telemetry, in prose instead of
// msgpack.

#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "NomadNet.h"

using namespace Rns;
using Rns::NomadNet::Status;

static Status running() {
  Status s;
  s.name = "retimesh-52A7F8";
  s.version = "v0.1.0";
  s.board = "LilyGO T3-S3";
  s.address = "d7c3f10a4b8e0357a1c9d4e6f80b2735";
  s.lxmfAddress = "2aa670cd82b918e2cf5457f46f5e3c44";
  s.uptimeS = 90061;                      // 1 d 1 h 1 m
  s.radioOnline = true;
  s.radioModel = "SX1276";
  s.freqMhz = 869.525f; s.bwKhz = 125.0f; s.sf = 8; s.cr = 5; s.txDbm = 7;
  s.heardAnything = true; s.lastRssi = -104.0f; s.lastSnr = 8.75f;
  s.rxPackets = 27; s.txPackets = 15;
  s.interfaces = 3; s.paths = 5; s.links = 1;
  s.lxmfRx = 9; s.lxmfUnverified = 0; s.lxmfMismatched = 0;
  s.haveBattery = true; s.batteryPct = 87; s.charging = true; s.chargeKnown = true;
  s.heapFree = 89156; s.heapLargest = 45044;
  return s;
}

static void test_a_page_names_the_node_and_where_to_reach_it() {
  char buf[1280];
  const size_t n = NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
  // A heading a browser will render, and the two addresses a person needs.
  TEST_ASSERT_NOT_NULL(strstr(buf, ">retimesh-52A7F8"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "2aa670cd82b918e2cf5457f46f5e3c44"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Up 1 d 1 h 1 m"));
}

static void test_a_page_reports_the_channel_it_is_actually_on() {
  char buf[1280];
  NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "SX1276 869.525 MHz BW125 SF8 CR4/5 7 dBm"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Heard -104 dBm / 8.8 dB SNR"));
}

// A live status readout is the one page that must never be cached. Without
// this header NomadNet keeps it for twelve hours and stops asking the node,
// so an operator would see the same uptime and heap all day.
static void test_the_page_tells_a_browser_not_to_cache_it() {
  char buf[1280];
  const size_t n = NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 5);
  // The header is only read if it is the first four bytes.
  TEST_ASSERT_EQUAL_STRING_LEN("#!c=0\n", buf, 6);
}

// A channel the chip refused is not stated as fact.
static void test_a_refused_channel_is_admitted() {
  Status s = running();
  s.channelRefused = true;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "refused"));
  NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "refused"));
}

// The node name is the one thing on this page an operator writes, and it is
// rendered in every visitor's browser. A backtick opens a micron escape and
// `>` heads a section, so neither may survive into the markup.
static void test_a_name_cannot_rewrite_the_page_it_appears_on() {
  Status s = running();
  s.name = "`[click`http://x] evil";
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NULL_MESSAGE(strchr(buf + 6, '`') == nullptr ? (void*)1 : nullptr,
                           "the page still uses backticks of its own");
  // What matters is that none of them came from the name: the first line
  // after the cache header is the heading, and it must carry neither.
  const char* heading = strstr(buf, ">");
  TEST_ASSERT_NOT_NULL(heading);
  const char* eol = strchr(heading, '\n');
  TEST_ASSERT_NOT_NULL(eol);
  for (const char* c = heading + 1; c < eol; c++)
    TEST_ASSERT_TRUE_MESSAGE(*c != '`' && *c != '>', "a name escaped into the markup");
}

// The budget is set by what leaves in one packet, and the generator lives in a
// different file from the buffer the firmware gives it. Without this, one more
// section would put every node in the field on a resource transfer — or on
// "(this page was cut short)" — with the suite still green.
static void test_an_ordinary_page_leaves_in_one_packet() {
  char buf[NomadNet::kPageMax];
  const size_t n = NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE_MESSAGE(n <= NomadNet::kOnePacketMax,
                           "an ordinary page must not cost a resource transfer");
}

// And the node with everything long about it still gets a whole page, even
// though it costs a resource to send. Cutting it to hold the packet budget
// would be the wrong trade.
static void test_the_longest_realistic_page_is_not_cut() {
  char buf[NomadNet::kPageMax];
  Status s = running();
  s.name = "retimesh-with-a-long-callsign-32";
  s.version = "v0.0.9-77-g72d2f1c-dirty";
  s.board = "Heltec Wireless Paper";
  s.uptimeS = 8640000;                       // 100 days, the widest figures
  s.rxPackets = 4000000000u; s.txPackets = 4000000000u;
  s.interfaces = 12; s.paths = 999; s.links = 99;
  s.lxmfRx = 999999; s.lxmfUnverified = 999999; s.lxmfMismatched = 999999;
  s.channelRefused = true;
  const size_t n = NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "the longest realistic page must still render");
  TEST_ASSERT_NULL_MESSAGE(strstr(buf, "cut short"),
                           "the buffer must be sized so a real node is never cut");
}

// A node that has heard nobody says so. Reporting 0 dBm reads as a very loud
// neighbour, which is the opposite of what it means.
static void test_a_node_that_has_heard_nothing_does_not_report_a_signal() {
  Status s = running();
  s.heardAnything = false;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Nothing heard yet"));
  TEST_ASSERT_NULL(strstr(buf, "Heard "));
}

static void test_a_radio_that_is_down_is_not_described_as_a_channel() {
  Status s = running();
  s.radioOnline = false;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "offline"));
  TEST_ASSERT_NULL(strstr(buf, "MHz"));
}

// The three standings stay apart on the page, as they do everywhere else:
// a sender the node never heard announce is not one whose signature failed.
static void test_the_page_keeps_the_standings_apart() {
  Status s = running();
  s.lxmfRx = 12; s.lxmfUnverified = 3; s.lxmfMismatched = 1;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "12 in, 3 unchecked, 1 mismatched"));
}

// A board that cannot see its charger says the charge and stops there.
static void test_a_charger_the_board_cannot_see_is_not_described() {
  Status s = running();
  s.chargeKnown = false;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Battery 87%"));
  TEST_ASSERT_NULL(strstr(buf, "discharging"));
  TEST_ASSERT_NULL(strstr(buf, "charging"));
}

static void test_a_board_with_no_cell_says_nothing_about_a_battery() {
  Status s = running();
  s.haveBattery = false;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "Battery"));
}

// The page is generated because a stranger asked, into a buffer on the task
// that runs the stack. At every size it must stay inside it, stay a string,
// and end on a whole line rather than mid-figure.
static void test_a_page_never_overruns_the_buffer_it_was_given() {
  const Status s = running();
  for (size_t cap = 0; cap < 1400; cap++) {
    char buf[1500];
    memset(buf, '#', sizeof(buf));
    const size_t n = NomadNet::index(s, buf, cap);
    TEST_ASSERT_TRUE_MESSAGE(n < cap || cap == 0, "wrote as much as it was given or more");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE('#', buf[cap], "wrote past the buffer");
    if (cap) TEST_ASSERT_EQUAL_size_t(n, strlen(buf));
  }
}

static void test_a_page_that_did_not_fit_says_so() {
  // Small enough to truncate a page that is now about 350 bytes.
  char buf[200];
  const size_t n = NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "cut short"),
                              "a truncated page must say it was truncated");
  // And it still ends on a line, not half a number.
  TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_page_names_the_node_and_where_to_reach_it);
  RUN_TEST(test_a_page_reports_the_channel_it_is_actually_on);
  RUN_TEST(test_the_page_tells_a_browser_not_to_cache_it);
  RUN_TEST(test_a_refused_channel_is_admitted);
  RUN_TEST(test_a_name_cannot_rewrite_the_page_it_appears_on);
  RUN_TEST(test_an_ordinary_page_leaves_in_one_packet);
  RUN_TEST(test_the_longest_realistic_page_is_not_cut);
  RUN_TEST(test_a_node_that_has_heard_nothing_does_not_report_a_signal);
  RUN_TEST(test_a_radio_that_is_down_is_not_described_as_a_channel);
  RUN_TEST(test_the_page_keeps_the_standings_apart);
  RUN_TEST(test_a_charger_the_board_cannot_see_is_not_described);
  RUN_TEST(test_a_board_with_no_cell_says_nothing_about_a_battery);
  RUN_TEST(test_a_page_never_overruns_the_buffer_it_was_given);
  RUN_TEST(test_a_page_that_did_not_fit_says_so);
  return UNITY_END();
}
