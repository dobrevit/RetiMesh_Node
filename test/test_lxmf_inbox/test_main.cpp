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
//  The inbox record format and the ring arithmetic, on the host.
//
//  These are the parts that decide whether a message read back is the message
//  that was stored. The file itself needs a filesystem and is exercised on the
//  node; what is here is everything that can be got wrong without one.
// ============================================================================
#include <unity.h>
#include <string.h>
#include "LxmfInbox.h"

using namespace Rns;

static InboxRecord sample(uint32_t seq, const char* text) {
  InboxRecord r{};
  r.seq = seq;
  r.bootId = 0xC0FFEE01;
  r.bootMs = 123456;
  r.sentAt = 1767225600.5;                    // a sender's clock, with a fraction
  for (int i = 0; i < 16; i++) r.from[i] = (uint8_t)(0xA0 + i);
  r.standing = StandingVerified;
  r.via = ViaLink;
  r.textLen = (uint16_t)strlen(text);
  memcpy(r.text, text, r.textLen);
  return r;
}

static void test_a_record_reads_back_as_what_was_written() {
  uint8_t buf[kInboxRecordSize];
  const InboxRecord in = sample(7, "hello from the bench");
  encodeInbox(in, buf);
  InboxRecord out{};
  TEST_ASSERT_TRUE(decodeInbox(buf, out));
  TEST_ASSERT_EQUAL_UINT32(in.seq, out.seq);
  TEST_ASSERT_EQUAL_UINT32(in.bootId, out.bootId);
  TEST_ASSERT_EQUAL_UINT32(in.bootMs, out.bootMs);
  // Bit for bit rather than approximately: this is a serialisation test, and
  // a timestamp that comes back close enough is a timestamp that came back
  // wrong. (Unity's double assertions are compiled out here anyway.)
  TEST_ASSERT_EQUAL_MEMORY(&in.sentAt, &out.sentAt, sizeof(double));
  TEST_ASSERT_EQUAL_MEMORY(in.from, out.from, 16);
  TEST_ASSERT_EQUAL_UINT8(in.standing, out.standing);
  TEST_ASSERT_EQUAL_UINT8(in.via, out.via);
  TEST_ASSERT_EQUAL_UINT16(in.textLen, out.textLen);
  TEST_ASSERT_EQUAL_MEMORY(in.text, out.text, in.textLen);
}

static void test_a_slot_that_was_never_written_is_not_a_message() {
  // A fresh file is fifty slots of zero, and the read walks all of them. Zero
  // has to mean "nothing here" rather than "a message from nobody at the
  // beginning of time", or a new node shows fifty blank rows.
  uint8_t zeros[kInboxRecordSize];
  memset(zeros, 0, sizeof(zeros));
  InboxRecord out{};
  TEST_ASSERT_FALSE(decodeInbox(zeros, out));
}

static void test_a_text_length_past_the_record_is_refused() {
  // The store is a file, and on the boards with a card it is a file on
  // something removable. A length that would read past the record is the
  // shape a truncated write or a foreign file takes, and copying it out is
  // how that becomes a crash rather than a missing row.
  uint8_t buf[kInboxRecordSize];
  encodeInbox(sample(3, "short"), buf);
  buf[38] = 0xFF; buf[39] = 0xFF;
  InboxRecord out{};
  TEST_ASSERT_FALSE(decodeInbox(buf, out));
}

static void test_a_standing_this_build_does_not_know_is_refused() {
  uint8_t buf[kInboxRecordSize];
  encodeInbox(sample(3, "short"), buf);
  buf[36] = 9;
  InboxRecord out{};
  TEST_ASSERT_FALSE(decodeInbox(buf, out));
}

static void test_text_longer_than_the_record_is_truncated_not_overrun() {
  // A message can be longer than the row that shows it. Keeping the first
  // part is the point: what matters is that it arrived and who from.
  InboxRecord r{};
  r.seq = 1;
  r.textLen = (uint16_t)(kInboxTextMax + 40);
  memset(r.text, 'x', kInboxTextMax);          // the struct holds no more than this
  uint8_t buf[kInboxRecordSize];
  encodeInbox(r, buf);
  InboxRecord out{};
  TEST_ASSERT_TRUE(decodeInbox(buf, out));
  TEST_ASSERT_EQUAL_UINT16(kInboxTextMax, out.textLen);
}

static void test_the_first_message_goes_in_the_first_slot() {
  // Sequence numbers start at one so that zero can mean an untouched slot.
  TEST_ASSERT_EQUAL_size_t(0, inboxSlot(1));
  TEST_ASSERT_EQUAL_size_t(1, inboxSlot(2));
  TEST_ASSERT_EQUAL_size_t(kInboxSlots - 1, inboxSlot((uint32_t)kInboxSlots));
}

static void test_the_ring_wraps_onto_the_oldest_slot() {
  // The fifty-first message overwrites the first, which is what makes this a
  // ring rather than a file that grows until the store is full.
  TEST_ASSERT_EQUAL_size_t(0, inboxSlot((uint32_t)kInboxSlots + 1));
  TEST_ASSERT_EQUAL_size_t(1, inboxSlot((uint32_t)kInboxSlots + 2));
  TEST_ASSERT_EQUAL_size_t(inboxSlot(7), inboxSlot(7 + (uint32_t)kInboxSlots));
}

static void test_the_slot_arithmetic_survives_a_sequence_that_wraps_at_32_bits() {
  // Four billion messages is not a real fleet, but the arithmetic is written
  // once and this is the case nobody would notice for years.
  const uint32_t last = 0xFFFFFFFF;
  TEST_ASSERT_TRUE(inboxSlot(last) < kInboxSlots);
  TEST_ASSERT_TRUE(inboxSlot(last - 1) < kInboxSlots);
}

static void test_the_three_standings_have_three_different_words() {
  // The console and the page both take their wording from here, so a message
  // cannot be described one way in one place and another way in the other.
  TEST_ASSERT_EQUAL_STRING("verified", standingName(StandingVerified));
  TEST_ASSERT_EQUAL_STRING("unverified", standingName(StandingNoKey));
  TEST_ASSERT_EQUAL_STRING("mismatch", standingName(StandingMismatch));
  TEST_ASSERT_EQUAL_STRING("packet", viaName(ViaPacket));
  TEST_ASSERT_EQUAL_STRING("link", viaName(ViaLink));
  TEST_ASSERT_EQUAL_STRING("resource", viaName(ViaResource));
}

static void test_a_record_is_the_size_the_slot_arithmetic_assumes() {
  // Every offset in the layout is written by hand; this is the one assertion
  // that the last field still fits inside the record it is written into.
  TEST_ASSERT_TRUE(40 + kInboxTextMax <= kInboxRecordSize);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_record_reads_back_as_what_was_written);
  RUN_TEST(test_a_slot_that_was_never_written_is_not_a_message);
  RUN_TEST(test_a_text_length_past_the_record_is_refused);
  RUN_TEST(test_a_standing_this_build_does_not_know_is_refused);
  RUN_TEST(test_text_longer_than_the_record_is_truncated_not_overrun);
  RUN_TEST(test_the_first_message_goes_in_the_first_slot);
  RUN_TEST(test_the_ring_wraps_onto_the_oldest_slot);
  RUN_TEST(test_the_slot_arithmetic_survives_a_sequence_that_wraps_at_32_bits);
  RUN_TEST(test_the_three_standings_have_three_different_words);
  RUN_TEST(test_a_record_is_the_size_the_slot_arithmetic_assumes);
  return UNITY_END();
}
