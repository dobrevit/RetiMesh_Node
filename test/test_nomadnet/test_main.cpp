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
  TEST_ASSERT_EQUAL_STRING_LEN(">retimesh-52A7F8", buf, 16);
  TEST_ASSERT_NOT_NULL(strstr(buf, "2aa670cd82b918e2cf5457f46f5e3c44"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "d7c3f10a4b8e0357a1c9d4e6f80b2735"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Up 1 d 1 h 1 m"));
}

static void test_a_page_reports_the_channel_it_is_actually_on() {
  char buf[1280];
  NomadNet::index(running(), buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "SX1276 at 869.525 MHz, 125 kHz, SF8, CR 4/5, 7 dBm"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Last heard at -104 dBm, 8.8 dB SNR"));
}

// A node that has heard nobody says so. Reporting 0 dBm reads as a very loud
// neighbour, which is the opposite of what it means.
static void test_a_node_that_has_heard_nothing_does_not_report_a_signal() {
  Status s = running();
  s.heardAnything = false;
  char buf[1280];
  NomadNet::index(s, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "Nothing heard yet"));
  TEST_ASSERT_NULL(strstr(buf, "Last heard at"));
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
  TEST_ASSERT_NOT_NULL(strstr(buf, "12 received"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "3 from a sender it could not check, 1 that did not match"));
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
  char buf[400];
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
  RUN_TEST(test_a_node_that_has_heard_nothing_does_not_report_a_signal);
  RUN_TEST(test_a_radio_that_is_down_is_not_described_as_a_channel);
  RUN_TEST(test_the_page_keeps_the_standings_apart);
  RUN_TEST(test_a_charger_the_board_cannot_see_is_not_described);
  RUN_TEST(test_a_board_with_no_cell_says_nothing_about_a_battery);
  RUN_TEST(test_a_page_never_overruns_the_buffer_it_was_given);
  RUN_TEST(test_a_page_that_did_not_fit_says_so);
  return UNITY_END();
}
