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

static void test_wifi_and_ppp_take_exactly_on_or_off() {
  TEST_ASSERT_EQUAL_STRING("OFF", parseOk("wifi off").args[0]);
  TEST_ASSERT_EQUAL_STRING("ON",  parseOk("WIFI on").args[0]);
  Request bad;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI maybe", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("WIFI ON OFF", bad));
  // PPP is the same switch for the other link a host reaches through this
  // port — typed on the port itself, before pppd takes it.
  TEST_ASSERT_EQUAL((int)Cmd::Ppp, (int)parseOk("ppp on").cmd);
  TEST_ASSERT_EQUAL_STRING("OFF", parseOk("PPP off").args[0]);
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("PPP", bad));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("PPP 115200", bad));
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
    if (a.feed(*p, over, 0)) {
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
  // The caller asks idle() when the port has nothing to read.
  LineAssembler a; bool over = false;
  for (const char* p = "AT"; *p; p++) a.feed(*p, over, 100);
  TEST_ASSERT_TRUE(a.pending());
  TEST_ASSERT_FALSE(a.idle(100 + LINE_IDLE_MS));        // not yet
  TEST_ASSERT_TRUE(a.idle(100 + LINE_IDLE_MS + 1));     // dropped
  TEST_ASSERT_FALSE(a.pending());
  uint32_t t = 100 + LINE_IDLE_MS + 2;
  for (const char* p = "VERSION"; *p; p++) a.feed(*p, over, t++);
  TEST_ASSERT_TRUE(a.feed('\n', over, t));
  TEST_ASSERT_EQUAL_STRING("VERSION", a.line());
  // Bytes still waiting to be read are not a stall, however late the loop
  // reads them: feed() never drops, only idle() does.
  LineAssembler b;
  for (const char* p = "VER"; *p; p++) b.feed(*p, over, 100);
  for (const char* p = "SION"; *p; p++) b.feed(*p, over, 100 + 3 * LINE_IDLE_MS);
  TEST_ASSERT_TRUE(b.feed('\n', over, 100 + 3 * LINE_IDLE_MS));
  TEST_ASSERT_EQUAL_STRING("VERSION", b.line());
  TEST_ASSERT_FALSE(b.idle(100 + 4 * LINE_IDLE_MS));    // nothing pending, nothing to drop
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
  for (size_t i = 0; i < sizeof(in); i++) { if (a.feed(in[i], over, 0)) lines++; if (over) reports++; }
  TEST_ASSERT_EQUAL(0, lines);
  TEST_ASSERT_EQUAL(0, reports);            // not until the line ends
  TEST_ASSERT_FALSE(a.feed('\n', over, 0));
  TEST_ASSERT_TRUE(over);
  // ...and the next line works.
  for (const char* p = "VERSION"; *p; p++) a.feed(*p, over, 0);
  TEST_ASSERT_TRUE(a.feed('\n', over, 0));
  TEST_ASSERT_EQUAL_STRING("VERSION", a.line());
}

static void test_an_overlong_line_that_pauses_is_still_swallowed_whole() {
  // The overlong line arrives in two writes with a stall between them: the
  // stall must not turn its tail into a fresh line that parses. idle()
  // drops what is buffered, not the fact that a line is being dropped.
  LineAssembler a;
  bool over = false;
  for (int i = 0; i < MAX_LINE + 10; i++) a.feed('X', over, 100);
  TEST_ASSERT_TRUE(a.pending());
  TEST_ASSERT_FALSE(a.idle(100 + 2 * LINE_IDLE_MS));      // nothing buffered to drop
  uint32_t t = 100 + 2 * LINE_IDLE_MS;
  int lines = 0;
  for (const char* p = "VERSION"; *p; p++) if (a.feed(*p, over, t++)) lines++;
  TEST_ASSERT_FALSE(a.feed('\n', over, t));
  TEST_ASSERT_EQUAL(0, lines);
  TEST_ASSERT_TRUE(over);                                  // reported once, at the newline
  TEST_ASSERT_FALSE(a.pending());
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

static void test_set_takes_its_value_as_typed() {
  // The tokens a command is matched by are uppercased, which is right for
  // ON|OFF and ruinous for a password: "MyPass123" that comes back as
  // "MYPASS123" authenticates against nothing. SET's value is therefore taken
  // from the line verbatim, spaces included, so an SSID with a space in it
  // survives the trip.
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("SET wifi.password MyPass123", r));
  TEST_ASSERT_EQUAL((int)Cmd::Set, (int)r.cmd);
  TEST_ASSERT_EQUAL_STRING("WIFI.PASSWORD", r.args[0]);      // the key, matched case-insensitively
  TEST_ASSERT_EQUAL(9, (int)r.rawValueLen);
  TEST_ASSERT_EQUAL(0, strncmp("MyPass123", r.rawValue, r.rawValueLen));

  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("SET wifi.ssid My Home Network  ", r));
  TEST_ASSERT_EQUAL(15, (int)r.rawValueLen);                 // trailing blanks trimmed, inner ones kept
  TEST_ASSERT_EQUAL(0, strncmp("My Home Network", r.rawValue, r.rawValueLen));

  // A key with nothing after it is a typo, not a request to blank a setting.
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("SET wifi.ssid", r));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("SET", r));

  // The longest key the table can name has to fit in an argument.
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("SET transport.announce_rate_penalty 3600", r));
  TEST_ASSERT_EQUAL_STRING("TRANSPORT.ANNOUNCE_RATE_PENALTY", r.args[0]);
}

static void test_get_reads_all_a_section_or_one_key() {
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("GET", r));
  TEST_ASSERT_EQUAL(0, (int)r.argc);
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("GET radio", r));
  TEST_ASSERT_EQUAL_STRING("RADIO", r.args[0]);
  TEST_ASSERT_EQUAL((int)ParseError::None, (int)parse("GET radio.sf", r));
  TEST_ASSERT_EQUAL_STRING("RADIO.SF", r.args[0]);
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("GET radio sf", r));
}

static void test_every_command_in_the_table_parses_by_its_own_name() {
  size_t n = 0;
  const CmdInfo* all = commands(n);
  TEST_ASSERT_TRUE(n >= 8);
  for (size_t i = 0; i < n; i++) {
    char line[64];
    // With its required argument where it has one, so the shape check passes.
    const char* arg = strcmp(all[i].args, "CONFIRM") == 0 ? " CONFIRM"
                    : strcmp(all[i].args, "ON|OFF") == 0  ? " ON"
                    : strcmp(all[i].args, "key value") == 0 ? " radio.sf 9"
                    : strcmp(all[i].args, "password") == 0 ? " hunter2" : "";
    snprintf(line, sizeof(line), "%s%s", all[i].name, arg);
    Request r;
    TEST_ASSERT_EQUAL_MESSAGE((int)ParseError::None, (int)parse(line, r), all[i].name);
    TEST_ASSERT_EQUAL((int)all[i].cmd, (int)r.cmd);
    TEST_ASSERT_EQUAL_STRING(all[i].name, cmdName(all[i].cmd));
  }
}

// --- AUTH, and the tail rule it shares with SET ------------------------------
static void test_auth_takes_the_whole_tail_with_its_case_and_spaces() {
  // The tokeniser uppercases, which is right for a command and fatal for a
  // password: "Sw0rdf1sh" arriving as "SW0RDF1SH" authenticates nobody. The
  // rule that spares SET's value spares this too, from one token earlier.
  Request r = parseOk("AUTH Sw0rdf1sh");
  TEST_ASSERT_EQUAL((int)Cmd::Auth, (int)r.cmd);
  TEST_ASSERT_EQUAL_size_t(9, r.rawValueLen);
  TEST_ASSERT_EQUAL_STRING_LEN("Sw0rdf1sh", r.rawValue, 9);

  // A password may contain spaces; the tail is taken whole, not tokenised.
  r = parseOk("AUTH two words here");
  TEST_ASSERT_EQUAL_size_t(14, r.rawValueLen);
  TEST_ASSERT_EQUAL_STRING_LEN("two words here", r.rawValue, 14);

  // Trailing whitespace is not part of the secret.
  r = parseOk("AUTH  padded   ");
  TEST_ASSERT_EQUAL_size_t(6, r.rawValueLen);
  TEST_ASSERT_EQUAL_STRING_LEN("padded", r.rawValue, 6);
}

static void test_auth_without_a_password_is_refused() {
  Request r;
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("AUTH", r));
  TEST_ASSERT_EQUAL((int)ParseError::BadArgument, (int)parse("AUTH   ", r));
}

static void test_set_still_takes_its_value_after_the_key() {
  // The tail rule was generalised to serve both; SET must not have moved.
  Request r = parseOk("SET wifi.sta_ssid My Network");
  TEST_ASSERT_EQUAL((int)Cmd::Set, (int)r.cmd);
  TEST_ASSERT_EQUAL_size_t(1, r.argc);
  TEST_ASSERT_EQUAL_STRING("WIFI.STA_SSID", r.args[0]);
  TEST_ASSERT_EQUAL_STRING_LEN("My Network", r.rawValue, 10);
}

static void test_an_unauthorised_reply_is_a_401_a_script_can_read() {
  TEST_ASSERT_EQUAL(401, errorCode(ParseError::Unauthorised));
  TEST_ASSERT_NOT_NULL(strstr(errorText(ParseError::Unauthorised), "AUTH"));
  char out[224];
  const size_t n = formatErr(out, sizeof(out), "STATUS", errorCode(ParseError::Unauthorised),
                             errorText(ParseError::Unauthorised));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING_LEN("RM ERR ", out, 7);
  TEST_ASSERT_NOT_NULL(strstr(out, " 401 "));
  TEST_ASSERT_NOT_NULL(strstr(out, "STATUS"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_commands_match_without_regard_to_case_or_whitespace);
  RUN_TEST(test_bootloader_and_reset_need_confirm_and_say_so);
  RUN_TEST(test_wifi_and_ppp_take_exactly_on_or_off);
  RUN_TEST(test_unknown_and_empty_lines_are_reported_not_ignored);
  RUN_TEST(test_commands_that_take_nothing_refuse_arguments);
  RUN_TEST(test_line_noise_is_not_a_command);
  RUN_TEST(test_overlong_tokens_and_lines_are_refused);
  RUN_TEST(test_the_assembler_delivers_lines_and_tolerates_crlf);
  RUN_TEST(test_a_line_that_stalls_is_dropped_not_prepended);
  RUN_TEST(test_an_overlong_line_is_dropped_whole_and_reported_once);
  RUN_TEST(test_an_overlong_line_that_pauses_is_still_swallowed_whole);
  RUN_TEST(test_replies_carry_the_prefix_a_host_filters_on);
  RUN_TEST(test_replies_never_overrun_a_small_buffer);
  RUN_TEST(test_set_takes_its_value_as_typed);
  RUN_TEST(test_get_reads_all_a_section_or_one_key);
  RUN_TEST(test_every_command_in_the_table_parses_by_its_own_name);
  RUN_TEST(test_auth_takes_the_whole_tail_with_its_case_and_spaces);
  RUN_TEST(test_auth_without_a_password_is_refused);
  RUN_TEST(test_set_still_takes_its_value_after_the_key);
  RUN_TEST(test_an_unauthorised_reply_is_a_401_a_script_can_read);
  return UNITY_END();
}
