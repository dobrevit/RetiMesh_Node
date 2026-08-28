// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// Where the Reticulum store lives. The rule is three inputs and two outcomes,
// which is exactly the size of thing that gets quietly rewritten wrong: the
// version before this one was written inline where the transport starts, and
// the only way to find out it had changed was to boot a node with a card in it
// and read the log.
//
// The case worth having tests for is the foreign card. Everything else is the
// setting getting its way; that one is the setting being overruled, and it is
// overruled to stop a node adopting another node's identity of record.

#include <unity.h>
#include "StoreHome.h"

using StoreHome::Where;
using StoreHome::Card;

static void test_the_setting_off_keeps_the_store_on_internal_flash() {
  TEST_ASSERT_TRUE(StoreHome::decide(false, true, Card::Ours)   == Where::LittleFs);
  TEST_ASSERT_TRUE(StoreHome::decide(false, true, Card::Blank)  == Where::LittleFs);
  TEST_ASSERT_TRUE(StoreHome::decide(false, true, Card::Legacy) == Where::LittleFs);
}

static void test_no_mounted_card_keeps_the_store_on_internal_flash() {
  TEST_ASSERT_TRUE(StoreHome::decide(true, false, Card::NoCard) == Where::LittleFs);
  // Even if a marker was read before the card went away: mounted is what counts,
  // because an unmounted card cannot be written to.
  TEST_ASSERT_TRUE(StoreHome::decide(true, false, Card::Ours)   == Where::LittleFs);
}

static void test_a_blank_card_is_taken_when_the_setting_asks_for_it() {
  // Preserves what the node did before markers existed: the setting says the
  // store belongs on a card, a card is present, so it goes there.
  TEST_ASSERT_TRUE(StoreHome::decide(true, true, Card::Blank) == Where::Sd);
}

static void test_our_own_card_is_taken() {
  TEST_ASSERT_TRUE(StoreHome::decide(true, true, Card::Ours) == Where::Sd);
}

static void test_a_store_without_a_marker_counts_as_ours() {
  // A card does not acquire a store by accident. One with files but no marker
  // was written by this node before markers existed, and refusing it would
  // strand the path table of every node already running.
  TEST_ASSERT_TRUE(StoreHome::decide(true, true, Card::Legacy) == Where::Sd);
}

static void test_another_nodes_card_is_refused_however_the_setting_is_set() {
  // The one case where the setting does not get its way.
  TEST_ASSERT_TRUE(StoreHome::decide(true,  true, Card::Foreign) == Where::LittleFs);
  TEST_ASSERT_TRUE(StoreHome::decide(false, true, Card::Foreign) == Where::LittleFs);
}

static void test_a_marker_starts_invalid_and_empty() {
  // The struct is read into by a parser that may fail halfway; callers check
  // `valid`, so the default has to be a safe one rather than whatever was on
  // the stack.
  StoreHome::Marker m;
  TEST_ASSERT_FALSE(m.valid);
  TEST_ASSERT_EQUAL_UINT32(0, m.generation);
  TEST_ASSERT_FALSE(m.released);
  TEST_ASSERT_EQUAL_STRING("", m.node);
  TEST_ASSERT_EQUAL_STRING("", m.name);
}

static void test_an_identity_hex_fits_the_marker_field() {
  // 32 hex characters plus a terminator. If the identity ever grows, this
  // fails here rather than silently truncating the owner's name on the card
  // and turning every node's own store into a foreign one.
  const char* identity = "00cea89545aed9f4d3a8a6c090bffa23";
  StoreHome::Marker m;
  TEST_ASSERT_TRUE(strlen(identity) < sizeof(m.node));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_setting_off_keeps_the_store_on_internal_flash);
  RUN_TEST(test_no_mounted_card_keeps_the_store_on_internal_flash);
  RUN_TEST(test_a_blank_card_is_taken_when_the_setting_asks_for_it);
  RUN_TEST(test_our_own_card_is_taken);
  RUN_TEST(test_a_store_without_a_marker_counts_as_ours);
  RUN_TEST(test_another_nodes_card_is_refused_however_the_setting_is_set);
  RUN_TEST(test_a_marker_starts_invalid_and_empty);
  RUN_TEST(test_an_identity_hex_fits_the_marker_field);
  return UNITY_END();
}
