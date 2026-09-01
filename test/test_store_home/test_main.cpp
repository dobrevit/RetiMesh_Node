// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// Where the Reticulum store lives. The rules are a handful of inputs and a
// couple of outcomes each, which is exactly the size of thing that gets
// quietly rewritten wrong: the version before this one was written inline
// where the transport starts, and the only way to find out it had changed was
// to boot a node with a card in it and read the log.
//
// The cases worth having tests for are the ones where the answer is not the
// obvious one: the foreign card, which is the setting being overruled to stop
// a node adopting another node's identity of record; the released card, which
// looks foreign and is not; and the blank card, which the setting wants and
// which is not a home until the data has been copied onto it.

#include <unity.h>
#include "StoreHome.h"

using StoreHome::Where;
using StoreHome::Card;
using StoreHome::Move;
using StoreHome::MarkerState;

// --- decide(): where the store is opened ------------------------------------

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

static void test_a_blank_card_is_not_a_home_until_the_store_is_on_it() {
  // The setting asking for a card is not the same as the card holding the
  // store. Opening an empty store on a blank card, with the real path table
  // still in flash, looks exactly like data loss to whoever is watching — so
  // the home stays where the data is until a copy has been made.
  TEST_ASSERT_TRUE(StoreHome::decide(true, true, Card::Blank) == Where::LittleFs);
  // ... and that copy is what the boot plan asks for.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None, true, true, Card::Blank) == Move::Adopt);
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

// --- classify(): what the card in the slot holds -----------------------------

static void test_an_unmounted_slot_holds_nothing() {
  TEST_ASSERT_TRUE(StoreHome::classify(false, MarkerState::Read, true, false, true) == Card::NoCard);
}

static void test_a_marked_card_is_ours_or_somebody_elses() {
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Read, true,  false, true) == Card::Ours);
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Read, false, false, true) == Card::Foreign);
}

static void test_a_released_card_is_free_for_any_node() {
  // The flag is the previous owner saying it has taken its store back and is
  // finished with the card. Comparing only the identity left an ejected card
  // foreign for ever: its own node could pick it up again because the name
  // matched, and every other node had to format a card whose owner had
  // already given it up.
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Read, false, true, false) == Card::Blank);
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Read, true,  true, false) == Card::Blank);
  // Even with a tree left behind on it: what a released card still holds is a
  // leftover, and the node that wrote it is working from its own copy.
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Read, false, true, true) == Card::Blank);
  // And it can then be taken, which is the whole point of consulting the flag.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true,
                                         StoreHome::classify(true, MarkerState::Read, false, true, false)) == Move::Adopt);
}

static void test_an_unmarked_card_is_legacy_only_when_it_holds_a_store() {
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Absent, false, false, true)  == Card::Legacy);
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Absent, false, false, false) == Card::Blank);
}

static void test_a_marker_that_cannot_be_read_is_never_ours() {
  // The marker is written with FILE_WRITE, which empties the file before
  // anything is serialised into it, so a power cut mid-write leaves a zero-byte
  // store.json. Read as "no marker" and found beside a store, that card was
  // classified legacy — ours — and the next boot signed it. On another node's
  // card that takes it for good, and the node that owned it comes back to a
  // card claimed by somebody else.
  //
  // Nothing about the identity is knowable from a file that will not parse, so
  // neither argument below is allowed to change the answer.
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Unreadable, false, false, true)  == Card::Foreign);
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Unreadable, true,  false, true)  == Card::Foreign);
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Unreadable, true,  true,  true)  == Card::Foreign);
  // Not even an empty one: an unreadable claim on a card with nothing on it is
  // still a claim, and formatting it is a decision for the person holding it.
  TEST_ASSERT_TRUE(StoreHome::classify(true, MarkerState::Unreadable, false, false, false) == Card::Foreign);
}

static void test_a_card_with_an_unreadable_marker_is_left_alone() {
  // Which is the whole point of calling it foreign: it is not opened as a home,
  // it is not offered, and a queued adopt does not take it either.
  const Card c = StoreHome::classify(true, MarkerState::Unreadable, true, false, true);
  TEST_ASSERT_TRUE(StoreHome::decide(true, true, c) == Where::LittleFs);
  TEST_ASSERT_FALSE(StoreHome::adoptable(c));
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true, c) == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None,  true, true, c) == Move::None);
}

// --- planAtBoot(): what is copied before the store is opened -----------------

static void test_a_move_needs_a_card_to_move_to_or_from() {
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, false, Card::NoCard) == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, true, false, Card::NoCard) == Move::None);
}

static void test_a_queued_adopt_is_refused_on_another_nodes_card() {
  // The card in the slot when the node comes back need not be the card the
  // request was made about.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true, Card::Foreign) == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true, Card::Blank)   == Move::Adopt);
  // A card of ours with the store still in flash — the setting says flash, so
  // that is where the data is — is taken back by copying onto it.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, false, true, Card::Ours)   == Move::Adopt);
}

static void test_nothing_is_copied_to_where_the_store_already_is() {
  // The guard against a stale idea of the home. With the setting on and a card
  // of ours in the slot the store is already on that card, so an adopt has
  // nothing to copy — and copying anyway means writing the flash tree, which is
  // whatever was left there before the card was taken, over the live one.
  //
  // This is not hypothetical: while the home was chosen inside the transport's
  // start-up, a node with the transport switched off never chose, reported
  // flash, and the settings page duly offered the adopt.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true, Card::Ours)   == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Adopt, true, true, Card::Legacy) == Move::None);
  // The same in the other direction: the store is not on the card unless the
  // setting says so, and an eject that reads from a card that is not the home
  // copies an old tree over the live one in flash.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, false, true, Card::Ours)   == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, false, true, Card::Legacy) == Move::None);
}

static void test_an_eject_needs_the_card_to_be_holding_the_store() {
  // Copying from a card that has no store on it would empty the flash copy
  // into nothing at all.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, true, true, Card::Ours)    == Move::Eject);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, true, true, Card::Legacy)  == Move::Eject);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, true, true, Card::Blank)   == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::Eject, true, true, Card::Foreign) == Move::None);
}

static void test_nothing_is_copied_when_nothing_was_asked_and_the_card_is_in_use() {
  // A card already carrying this node's store is simply opened.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None, true, true, Card::Ours)   == Move::None);
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None, true, true, Card::Legacy) == Move::None);
  // And a blank card is left alone when the setting does not want it.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None, false, true, Card::Blank) == Move::None);
  // A card belonging to somebody else is never taken by default.
  TEST_ASSERT_TRUE(StoreHome::planAtBoot(Move::None, true, true, Card::Foreign) == Move::None);
}

static void test_a_card_is_offered_unless_it_belongs_to_somebody_else() {
  TEST_ASSERT_FALSE(StoreHome::adoptable(Card::NoCard));
  TEST_ASSERT_FALSE(StoreHome::adoptable(Card::Foreign));
  TEST_ASSERT_TRUE (StoreHome::adoptable(Card::Blank));
  TEST_ASSERT_TRUE (StoreHome::adoptable(Card::Ours));
  TEST_ASSERT_TRUE (StoreHome::adoptable(Card::Legacy));
}

// --- what may be offered -----------------------------------------------------

static void test_an_eject_is_not_offered_once_the_card_is_gone() {
  // Pulling the card leaves the store nominally on it — the home cannot change
  // while the node is running — so "the store is on the card" stays true and
  // used to be the whole rule. The eject it allowed cost a restart and then
  // failed for want of a card to read the store off.
  TEST_ASSERT_NULL(StoreHome::ejectRefusal(false, Where::Sd, false));
  TEST_ASSERT_NOT_NULL(StoreHome::ejectRefusal(false, Where::Sd, true));
  TEST_ASSERT_NOT_NULL(StoreHome::ejectRefusal(false, Where::LittleFs, false));
  // And nothing is offered while a move is already on the books.
  TEST_ASSERT_NOT_NULL(StoreHome::ejectRefusal(true, Where::Sd, false));
}

static void test_an_adopt_is_offered_for_a_card_this_node_may_take() {
  TEST_ASSERT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Blank,  false));
  TEST_ASSERT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Ours,   false));
  TEST_ASSERT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Legacy, false));
  TEST_ASSERT_NOT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Foreign, false));
  TEST_ASSERT_NOT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::NoCard,  false));
  // Already there, already queued, or the card is gone: all three are answers
  // the page needs before it lights the button, not after the request.
  TEST_ASSERT_NOT_NULL(StoreHome::adoptRefusal(false, Where::Sd,       Card::Ours,  false));
  TEST_ASSERT_NOT_NULL(StoreHome::adoptRefusal(true,  Where::LittleFs, Card::Blank, false));
  TEST_ASSERT_NOT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Blank, true));
}

// --- the marker --------------------------------------------------------------

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
  // The length comes from kIdentityHexLen, which StoreHome.cpp static_asserts
  // against 2 * Rns::HASH_LEN — so an identity that grows fails there, and a
  // marker field that shrinks fails here. Comparing a hardcoded literal
  // against sizeof, as this did, could not fail either way: both numbers were
  // written by the same hand at the same moment.
  StoreHome::Marker m;
  TEST_ASSERT_TRUE(StoreHome::kIdentityHexLen + 1 <= sizeof(m.node));
}

// --- what a node says about where its store ended up ------------------------

static void test_a_card_in_the_slot_that_is_not_used_is_explained() {
  // The case that cost an afternoon: a mounted card, a store in 128 KiB of
  // flash, and nothing anywhere saying why.
  const char* why = StoreHome::flashReason(true, true, Card::Foreign);
  TEST_ASSERT_NOT_NULL(why);
  TEST_ASSERT_NOT_NULL(strstr(why, "format"));
}

static void test_a_store_on_the_card_needs_no_explanation() {
  TEST_ASSERT_NULL(StoreHome::flashReason(true, true, Card::Ours));
  TEST_ASSERT_NULL(StoreHome::flashReason(true, true, Card::Legacy));
}

static void test_each_reason_for_flash_is_its_own_sentence() {
  const char* noCard   = StoreHome::flashReason(true,  false, Card::NoCard);
  const char* setting  = StoreHome::flashReason(false, true,  Card::Ours);
  const char* foreign  = StoreHome::flashReason(true,  true,  Card::Foreign);
  const char* notReady = StoreHome::flashReason(true,  true,  Card::Blank);
  const char* all[] = {noCard, setting, foreign, notReady};
  for (const char* r : all) TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(noCard, setting));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(setting, foreign));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(foreign, notReady));
}

static void test_a_card_this_node_wrote_under_an_older_identity_says_so() {
  // Same refusal either way — the store on it cannot be opened by an identity
  // that did not write it — but the operator is standing in front of the node
  // whose name is on the card, and "another node's store" is not what they see.
  const char* mine   = StoreHome::adoptRefusal(false, Where::LittleFs, Card::Foreign, false, true);
  const char* theirs = StoreHome::adoptRefusal(false, Where::LittleFs, Card::Foreign, false, false);
  TEST_ASSERT_NOT_NULL(mine);
  TEST_ASSERT_NOT_NULL(theirs);
  TEST_ASSERT_NOT_EQUAL(0, strcmp(mine, theirs));
  TEST_ASSERT_NOT_NULL(strstr(mine, "earlier identity"));
  TEST_ASSERT_NOT_NULL(strstr(theirs, "another node"));
  TEST_ASSERT_FALSE(StoreHome::adoptable(Card::Foreign));   // neither takes it
}

static void test_the_older_identity_wording_is_only_for_a_foreign_card() {
  // Nothing else in the rule changes because the name happens to match.
  TEST_ASSERT_EQUAL_STRING(StoreHome::adoptRefusal(true, Where::LittleFs, Card::Ours, false, false),
                           StoreHome::adoptRefusal(true, Where::LittleFs, Card::Ours, false, true));
  TEST_ASSERT_EQUAL_STRING(StoreHome::adoptRefusal(false, Where::Sd, Card::Ours, false, false),
                           StoreHome::adoptRefusal(false, Where::Sd, Card::Ours, false, true));
  TEST_ASSERT_NULL(StoreHome::adoptRefusal(false, Where::LittleFs, Card::Blank, false, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_setting_off_keeps_the_store_on_internal_flash);
  RUN_TEST(test_no_mounted_card_keeps_the_store_on_internal_flash);
  RUN_TEST(test_a_blank_card_is_not_a_home_until_the_store_is_on_it);
  RUN_TEST(test_our_own_card_is_taken);
  RUN_TEST(test_a_store_without_a_marker_counts_as_ours);
  RUN_TEST(test_another_nodes_card_is_refused_however_the_setting_is_set);
  RUN_TEST(test_an_unmounted_slot_holds_nothing);
  RUN_TEST(test_a_marked_card_is_ours_or_somebody_elses);
  RUN_TEST(test_a_released_card_is_free_for_any_node);
  RUN_TEST(test_an_unmarked_card_is_legacy_only_when_it_holds_a_store);
  RUN_TEST(test_a_marker_that_cannot_be_read_is_never_ours);
  RUN_TEST(test_a_card_with_an_unreadable_marker_is_left_alone);
  RUN_TEST(test_a_move_needs_a_card_to_move_to_or_from);
  RUN_TEST(test_a_queued_adopt_is_refused_on_another_nodes_card);
  RUN_TEST(test_nothing_is_copied_to_where_the_store_already_is);
  RUN_TEST(test_an_eject_needs_the_card_to_be_holding_the_store);
  RUN_TEST(test_nothing_is_copied_when_nothing_was_asked_and_the_card_is_in_use);
  RUN_TEST(test_a_card_is_offered_unless_it_belongs_to_somebody_else);
  RUN_TEST(test_an_eject_is_not_offered_once_the_card_is_gone);
  RUN_TEST(test_an_adopt_is_offered_for_a_card_this_node_may_take);
  RUN_TEST(test_a_marker_starts_invalid_and_empty);
  RUN_TEST(test_an_identity_hex_fits_the_marker_field);
  RUN_TEST(test_a_card_in_the_slot_that_is_not_used_is_explained);
  RUN_TEST(test_a_store_on_the_card_needs_no_explanation);
  RUN_TEST(test_each_reason_for_flash_is_its_own_sentence);
  RUN_TEST(test_a_card_this_node_wrote_under_an_older_identity_says_so);
  RUN_TEST(test_the_older_identity_wording_is_only_for_a_foreign_card);
  return UNITY_END();
}
