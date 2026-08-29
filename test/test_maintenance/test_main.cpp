// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The maintenance console's parser, line assembler and reply format. The
// port this runs on also carries the log, line noise from a bridge, and
// whatever a person types, so most of what matters is what the parser
// refuses — and that it refuses out loud, with a reply a script can read.

#include <unity.h>
#include "MaintenanceProtocol.h"

using namespace Maintenance;

static Request parseOk(const char* line) {
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse(line, r));
  return r;
}

static void test_commands_match_without_regard_to_case_or_whitespace() {
  TEST_ASSERT_EQUAL((int)Cmd::Version, (int)parseOk("version").cmd);
  TEST_ASSERT_EQUAL((int)Cmd::Version, (int)parseOk("  VERSION \r").cmd);
  TEST_ASSERT_EQUAL((int)Cmd::Status,  (int)parseOk("Status").cmd);
  TEST_ASSERT_EQUAL((int)Cmd::NetworkStatus, (int)parseOk("network_status").cmd);
}

static void test_bootloader_and_reset_need_confirm_and_say_so() {
  Request r = parseOk("bootloader");
  TEST_ASSERT_EQUAL((int)Cmd::Bootloader, (int)r.cmd);
  TEST_ASSERT_FALSE(r.confirmed);            // parses, but the handler must refuse
  r = parseOk("BOOTLOADER confirm");
  TEST_ASSERT_TRUE(r.confirmed);
  r = parseOk("reset CONFIRM");
  TEST_ASSERT_EQUAL((int)Cmd::Reset, (int)r.cmd);
  TEST_ASSERT_TRUE(r.confirmed);
  // "BOOTLOADER YES" is a person guessing, and guessing must not work.
  Request bad;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("BOOTLOADER YES", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("BOOTLOADER CONFIRM NOW", bad));
}

static void test_wifi_takes_exactly_on_or_off() {
  TEST_ASSERT_EQUAL_STRING("OFF", parseOk("wifi off").args[0]);
  TEST_ASSERT_EQUAL_STRING("ON",  parseOk("WIFI on").args[0]);
  Request bad;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI maybe", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI ON OFF", bad));
}

static void test_unknown_and_empty_lines_are_reported_not_ignored() {
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::Unknown, (int)parse("FLASH", r));
  TEST_ASSERT_EQUAL_STRING("FLASH", r.word);   // so the reply can name it
  TEST_ASSERT_EQUAL((int)ParseError::Empty, (int)parse("", r));
  TEST_ASSERT_EQUAL((int)ParseError::Empty, (int)parse("   \r\n", r));
  TEST_ASSERT_EQUAL((int)ParseError::Empty, (int)parse(nullptr, r));
  TEST_ASSERT_EQUAL(404, errorCode(ParseError::Unknown));
  TEST_ASSERT_EQUAL(400, errorCode(ParseError::BadArgument));
}

static void test_commands_that_take_nothing_refuse_arguments() {
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("VERSION please", r));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("STATUS 1 2 3 4", r));
}

static void test_line_noise_is_not_a_command() {
  // A bridge at the wrong baud produces bytes outside printable ASCII. None
  // of them may be uppercased into something that happens to match.
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("VERSI\xd6N", r));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("\x01\x02STATUS", r));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("STATUS\x7f", r));
}

static void test_overlong_tokens_and_lines_are_refused() {
  Request r;
  char tooLong[MAX_LINE + 2];
  memset(tooLong, 'A', sizeof(tooLong) - 1);
  tooLong[sizeof(tooLong) - 1] = '\0';
  TEST_ASSERT_EQUAL((int)ParseError::TooLong, (int)parse(tooLong, r));
  // A token longer than an argument slot, on a line that is short enough.
  char longArg[MAX_ARG + 8] = "WIFI ";
  memset(longArg + 5, 'O', sizeof(longArg) - 6);
  longArg[sizeof(longArg) - 1] = '\0';
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse(longArg, r));
}

// --- LineAssembler -----------------------------------------------------------

static void test_the_assembler_delivers_lines_and_tolerates_crlf() {
  LineAssembler a;
  bool over = false;
  const char* in = "VERSION\r\nSTATUS\n\n\r\n";
  int lines = 0;
  for (const char* p = in; *p; p++) {
    if (a.feed(*p, over)) {
      lines++;
      if (lines == 1) TEST_ASSERT_EQUAL_STRING("VERSION", a.line());
      if (lines == 2) TEST_ASSERT_EQUAL_STRING("STATUS", a.line());
    }
    TEST_ASSERT_FALSE(over);
  }
  TEST_ASSERT_EQUAL(2, lines);
}

static void test_a_line_that_stalls_is_dropped_not_prepended() {
  // A port prober wrote "AT" and went away; two seconds later a real command
  // arrives and must not become "ATVERSION".
  LineAssembler a; bool over = false;
  for (const char* p = "AT"; *p; p++) a.feed(*p, over, 100);
  TEST_ASSERT_TRUE(a.pending());
  uint32_t t = 100 + LINE_IDLE_MS + 1;
  for (const char* p = "VERSION"; *p; p++) a.feed(*p, over, t++);
  TEST_ASSERT_TRUE(a.feed('\n', over, t));
  TEST_ASSERT_EQUAL_STRING("VERSION", a.line());
  TEST_ASSERT_FALSE(a.pending());
  // Within the window the bytes are one line, as typed.
  LineAssembler b;
  for (const char* p = "VER"; *p; p++) b.feed(*p, over, 100);
  for (const char* p = "SION"; *p; p++) b.feed(*p, over, 100 + LINE_IDLE_MS - 1);
  TEST_ASSERT_TRUE(b.feed('\n', over, 100 + LINE_IDLE_MS));
  TEST_ASSERT_EQUAL_STRING("VERSION", b.line());
}

static void test_an_overlong_line_is_dropped_whole_and_reported_once() {
  // Never truncated into a shorter line that might parse: "BOOTLOADER
  // CONFIRM" followed by 90 bytes of garbage must not become "BOOTLOADER
  // CONFIRM".
  LineAssembler a;
  bool over = false;
  int reports = 0, lines = 0;
  char in[MAX_LINE + 40];
  memset(in, 'X', sizeof(in));
  memcpy(in, "BOOTLOADER CONFIRM ", 19);
  for (size_t i = 0; i < sizeof(in); i++) { if (a.feed(in[i], over)) lines++; if (over) reports++; }
  TEST_ASSERT_EQUAL(0, lines);
  TEST_ASSERT_EQUAL(0, reports);            // not until the line ends
  TEST_ASSERT_FALSE(a.feed('\n', over));
  TEST_ASSERT_TRUE(over);
  // ...and the next line works.
  for (const char* p = "VERSION"; *p; p++) a.feed(*p, over);
  TEST_ASSERT_TRUE(a.feed('\n', over));
  TEST_ASSERT_EQUAL_STRING("VERSION", a.line());
}

// --- replies -----------------------------------------------------------------

static void test_replies_carry_the_prefix_a_host_filters_on() {
  char buf[128];
  // The count of data lines comes first on every OK line, with or without
  // command-specific pairs after it.
  formatOk(buf, sizeof(buf), "VERSION", 1);
  TEST_ASSERT_EQUAL_STRING("RM OK VERSION lines=1", buf);
  formatOk(buf, sizeof(buf), "BOOTLOADER", 0, "method=software_api delay_ms=300");
  TEST_ASSERT_EQUAL_STRING("RM OK BOOTLOADER lines=0 method=software_api delay_ms=300", buf);
  formatData(buf, sizeof(buf), "VERSION", "version=v1.0.0");
  TEST_ASSERT_EQUAL_STRING("RM VERSION version=v1.0.0", buf);
  formatErr(buf, sizeof(buf), "FLASH", 404, "unknown command, try HELP");
  TEST_ASSERT_EQUAL_STRING("RM ERR FLASH 404 unknown command, try HELP", buf);
  // Every reply begins with the prefix the host filters on, and the prefix
  // ends in the space that keeps "RMX" from matching.
  TEST_ASSERT_EQUAL_STRING_LEN("RM ", buf, 3);
}

static void test_replies_never_overrun_a_small_buffer() {
  char buf[16];
  const size_t n = formatErr(buf, sizeof(buf), "NETWORK_STATUS", 501, "this board has no USB network");
  TEST_ASSERT_TRUE(n >= sizeof(buf));       // snprintf reports what it wanted
  TEST_ASSERT_EQUAL('\0', buf[sizeof(buf) - 1]);
}

static void test_every_command_in_the_table_parses_by_its_own_name() {
  size_t n = 0;
  const CmdInfo* all = commands(n);
  TEST_ASSERT_TRUE(n >= 8);
  for (size_t i = 0; i < n; i++) {
    char line[64];
    // With its required argument where it has one, so the shape check passes.
    const char* arg = strcmp(all[i].args, "CONFIRM") == 0 ? " CONFIRM"
                    : strcmp(all[i].args, "ON|OFF") == 0  ? " ON" : "";
    snprintf(line, sizeof(line), "%s%s", all[i].name, arg);
    Request r;
    TEST_ASSERT_EQUAL_MESSAGE((int)ParseError::None, (int)parse(line, r), all[i].name);
    TEST_ASSERT_EQUAL((int)all[i].cmd, (int)r.cmd);
    TEST_ASSERT_EQUAL_STRING(all[i].name, cmdName(all[i].cmd));
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_commands_match_without_regard_to_case_or_whitespace);
  RUN_TEST(test_bootloader_and_reset_need_confirm_and_say_so);
  RUN_TEST(test_wifi_takes_exactly_on_or_off);
  RUN_TEST(test_unknown_and_empty_lines_are_reported_not_ignored);
  RUN_TEST(test_commands_that_take_nothing_refuse_arguments);
  RUN_TEST(test_line_noise_is_not_a_command);
  RUN_TEST(test_overlong_tokens_and_lines_are_refused);
  RUN_TEST(test_the_assembler_delivers_lines_and_tolerates_crlf);
  RUN_TEST(test_a_line_that_stalls_is_dropped_not_prepended);
  RUN_TEST(test_an_overlong_line_is_dropped_whole_and_reported_once);
  RUN_TEST(test_replies_carry_the_prefix_a_host_filters_on);
  RUN_TEST(test_replies_never_overrun_a_small_buffer);
  RUN_TEST(test_every_command_in_the_table_parses_by_its_own_name);
  return UNITY_END();
}
