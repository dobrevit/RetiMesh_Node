// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

// ============================================================================
//  Who may command a node, decided on a host where it can be argued with.
//
//  Every test here is a way in that must stay shut. The delivery address is
//  reachable by anyone who can route to it, so each of these is something a
//  stranger can actually try.
// ============================================================================
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include "RnsAdmin.h"
#include "LxmfInbox.h"

using namespace Rns;
using namespace Rns::Admin;

static const char* kAdminHex = "aabbccddeeff00112233445566778899";
static const char* kOtherHex = "0123456789abcdef0123456789abcdef";

static void fill(uint8_t out[16], const char* hex) {
  for (int i = 0; i < 16; i++) hexByte(hex + i * 2, out[i]);
}

static List oneAdmin() {
  List l{};
  TEST_ASSERT_TRUE(parseAdmins(kAdminHex, l));
  return l;
}

static Caller caller(const uint8_t* src, uint8_t standing, double sentAt, size_t len = 6) {
  Caller c;
  c.standing = standing;
  c.source = src;
  c.sentAt = sentAt;
  c.textLen = len;
  return c;
}

// --- the switch -------------------------------------------------------------

static void test_remote_administration_is_off_until_it_is_turned_on() {
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(Disabled, judge(false, l, caller(src, StandingVerified, 100), StandingVerified, which));
}

static void test_an_empty_list_is_off_however_the_switch_reads() {
  // Turning the feature on without naming anybody must not mean "anyone".
  uint8_t src[16]; fill(src, kAdminHex);
  List empty{};
  size_t which = 0;
  TEST_ASSERT_EQUAL(Disabled, judge(true, empty, caller(src, StandingVerified, 100), StandingVerified, which));
}

// --- who ---------------------------------------------------------------------

static void test_a_sender_with_no_key_to_check_cannot_command() {
  // The common case, not the rare one: most senders have never announced to
  // this node. Their message is still read and shown; it is not obeyed.
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(NotVerified, judge(true, l, caller(src, StandingNoKey, 100), StandingVerified, which));
}

static void test_a_signature_that_did_not_match_cannot_command() {
  // The dangerous one. A source hash is a claim anybody can put in a field,
  // so the hash matching the list means nothing on its own — this is the case
  // where somebody has claimed to be the administrator and been caught.
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(NotVerified, judge(true, l, caller(src, StandingMismatch, 100), StandingVerified, which));
}

static void test_a_proved_stranger_is_still_a_stranger() {
  // Verification proves who, never what they may do. Anyone at all can
  // announce and be verified; the list is the authorisation.
  uint8_t other[16]; fill(other, kOtherHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(NotAdmin, judge(true, l, caller(other, StandingVerified, 100), StandingVerified, which));
}

static void test_the_administrator_gets_through_and_is_identified() {
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 99;
  TEST_ASSERT_EQUAL(Allowed, judge(true, l, caller(src, StandingVerified, 100), StandingVerified, which));
  TEST_ASSERT_EQUAL_size_t(0, which);
}

// --- replay -------------------------------------------------------------------

static void test_the_same_message_sent_again_is_refused() {
  // A signed message stays signed. Anyone who kept a copy off the air can
  // send it again without holding any key at all, so "it verified" is not
  // enough on its own — it has to be new.
  uint8_t src[16]; fill(src, kAdminHex);
  List l = oneAdmin();
  l.lastSeen[0] = 100;
  size_t which = 0;
  TEST_ASSERT_EQUAL(Replayed, judge(true, l, caller(src, StandingVerified, 100), StandingVerified, which));
}

static void test_an_older_message_is_refused() {
  uint8_t src[16]; fill(src, kAdminHex);
  List l = oneAdmin();
  l.lastSeen[0] = 500;
  size_t which = 0;
  TEST_ASSERT_EQUAL(Replayed, judge(true, l, caller(src, StandingVerified, 499.9), StandingVerified, which));
}

static void test_a_message_with_no_clock_is_refused_once_one_has_been_seen() {
  // A sender that omits the timestamp reads as zero, which is never newer
  // than anything already accepted. That is the right answer: a command with
  // no clock cannot be told from a replay of itself.
  uint8_t src[16]; fill(src, kAdminHex);
  List l = oneAdmin();
  l.lastSeen[0] = 1;
  size_t which = 0;
  TEST_ASSERT_EQUAL(Replayed, judge(true, l, caller(src, StandingVerified, 0), StandingVerified, which));
}

static void test_replay_is_tracked_per_administrator() {
  // One admin's traffic must not raise the bar for another's, or the second
  // person to send a command is refused for no reason they can see.
  char both[80];
  snprintf(both, sizeof(both), "%s,%s", kAdminHex, kOtherHex);
  List l{};
  TEST_ASSERT_TRUE(parseAdmins(both, l));
  TEST_ASSERT_EQUAL_size_t(2, l.count);
  l.lastSeen[0] = 900;                       // the first has been busy
  uint8_t other[16]; fill(other, kOtherHex);
  size_t which = 99;
  TEST_ASSERT_EQUAL(Allowed, judge(true, l, caller(other, StandingVerified, 10), StandingVerified, which));
  TEST_ASSERT_EQUAL_size_t(1, which);
}

// --- the command itself --------------------------------------------------------

static void test_a_message_with_nothing_in_it_is_not_a_command() {
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(Empty, judge(true, l, caller(src, StandingVerified, 100, 0), StandingVerified, which));
}

static void test_a_caller_with_no_source_is_refused() {
  const List l = oneAdmin();
  size_t which = 0;
  Caller c = caller(nullptr, StandingVerified, 100);
  TEST_ASSERT_EQUAL(NotAdmin, judge(true, l, c, StandingVerified, which));
}

// --- the list -------------------------------------------------------------------

static void test_a_list_reads_back_as_the_bytes_it_names() {
  List l = oneAdmin();
  TEST_ASSERT_EQUAL_size_t(1, l.count);
  uint8_t expect[16]; fill(expect, kAdminHex);
  TEST_ASSERT_EQUAL_MEMORY(expect, l.hash[0], 16);
  TEST_ASSERT_TRUE(l.lastSeen[0] == 0.0);
}

static void test_an_empty_setting_is_an_empty_list_not_an_error() {
  List l{};
  TEST_ASSERT_TRUE(parseAdmins("", l));
  TEST_ASSERT_EQUAL_size_t(0, l.count);
  TEST_ASSERT_TRUE(parseAdmins(nullptr, l));
  TEST_ASSERT_EQUAL_size_t(0, l.count);
}

static void test_several_administrators_separated_by_commas() {
  char both[80];
  snprintf(both, sizeof(both), "%s, %s", kAdminHex, kOtherHex);
  List l{};
  TEST_ASSERT_TRUE(parseAdmins(both, l));
  TEST_ASSERT_EQUAL_size_t(2, l.count);
}

static void test_a_malformed_list_is_refused_whole_not_partly_accepted() {
  // A list that quietly dropped what it could not read would be shorter than
  // the operator believes, and they would not find out until the day it
  // mattered.
  List l{};
  TEST_ASSERT_FALSE(parseAdmins("nothex", l));
  TEST_ASSERT_FALSE(parseAdmins("aabb", l));                       // too short
  char bad[80];
  snprintf(bad, sizeof(bad), "%s,zz", kAdminHex);
  TEST_ASSERT_FALSE(parseAdmins(bad, l));
  snprintf(bad, sizeof(bad), "%sff", kAdminHex);                   // 34 digits, no separator
  TEST_ASSERT_FALSE(parseAdmins(bad, l));
}

static void test_more_administrators_than_the_node_holds_is_refused() {
  char many[200] = "";
  for (int i = 0; i < 5; i++) {
    strcat(many, kAdminHex);
    if (i < 4) strcat(many, ",");
  }
  List l{};
  TEST_ASSERT_FALSE(parseAdmins(many, l));
}

static void test_every_refusal_has_words_of_its_own() {
  // The console prints these to an operator whose command did nothing. Four
  // refusals that all read "refused" would send them looking in the wrong
  // place, which is the whole reason the verdicts are separate.
  TEST_ASSERT_EQUAL_STRING("allowed", verdictName(Allowed));
  TEST_ASSERT_EQUAL_STRING("disabled", verdictName(Disabled));
  TEST_ASSERT_EQUAL_STRING("unverified sender", verdictName(NotVerified));
  TEST_ASSERT_EQUAL_STRING("not an administrator", verdictName(NotAdmin));
  TEST_ASSERT_EQUAL_STRING("replayed", verdictName(Replayed));
  TEST_ASSERT_EQUAL_STRING("empty command", verdictName(Empty));
  TEST_ASSERT_EQUAL_STRING("command too long", verdictName(TooLong));
  TEST_ASSERT_EQUAL_STRING("the console is switched off", verdictName(NoConsole));
}

static void test_a_command_longer_than_a_command_line_is_refused_not_trimmed() {
  // The console's line limit is the same number, so a command cut down to fit
  // parses perfectly and runs. "SET admin.password <long passphrase>" would
  // then set a password neither the sender nor the operator knows, and answer
  // OK. What runs has to be what was sent, or nothing.
  uint8_t src[16]; fill(src, kAdminHex);
  const List l = oneAdmin();
  size_t which = 0;
  TEST_ASSERT_EQUAL(TooLong, judge(true, l, caller(src, StandingVerified, 100, kMaxCommand + 1),
                                   StandingVerified, which));
  TEST_ASSERT_EQUAL(Allowed, judge(true, l, caller(src, StandingVerified, 100, kMaxCommand),
                                   StandingVerified, which));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_remote_administration_is_off_until_it_is_turned_on);
  RUN_TEST(test_an_empty_list_is_off_however_the_switch_reads);
  RUN_TEST(test_a_sender_with_no_key_to_check_cannot_command);
  RUN_TEST(test_a_signature_that_did_not_match_cannot_command);
  RUN_TEST(test_a_proved_stranger_is_still_a_stranger);
  RUN_TEST(test_the_administrator_gets_through_and_is_identified);
  RUN_TEST(test_the_same_message_sent_again_is_refused);
  RUN_TEST(test_an_older_message_is_refused);
  RUN_TEST(test_a_message_with_no_clock_is_refused_once_one_has_been_seen);
  RUN_TEST(test_replay_is_tracked_per_administrator);
  RUN_TEST(test_a_message_with_nothing_in_it_is_not_a_command);
  RUN_TEST(test_a_caller_with_no_source_is_refused);
  RUN_TEST(test_a_list_reads_back_as_the_bytes_it_names);
  RUN_TEST(test_an_empty_setting_is_an_empty_list_not_an_error);
  RUN_TEST(test_several_administrators_separated_by_commas);
  RUN_TEST(test_a_malformed_list_is_refused_whole_not_partly_accepted);
  RUN_TEST(test_more_administrators_than_the_node_holds_is_refused);
  RUN_TEST(test_a_command_longer_than_a_command_line_is_refused_not_trimmed);
  RUN_TEST(test_every_refusal_has_words_of_its_own);
  return UNITY_END();
}
