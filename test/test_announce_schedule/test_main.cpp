// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// When the node announces next.
//
// This arithmetic used to live inside the task loop, where nothing could reach
// it: the wrap, the jitter bound and the promotion of a uint16 interval into
// milliseconds were all unexercised, and the jitter was one-sided for as long
// as it took someone to read it.

#include <unity.h>
#include "RnsAnnounce.h"

using namespace Rns;

// The configured interval is what the node should average. Adding jitter and
// never subtracting it made it a floor instead: every announce late, by up to
// a tenth of the interval, for ever.
static void test_the_jitter_is_centred_on_the_interval() {
  const uint16_t interval = 60;                     // seconds
  const uint32_t now = 1000, base = 60000;
  uint64_t total = 0;
  uint32_t lo = 0xFFFFFFFF, hi = 0;
  for (uint32_t r = 0; r < 20000; r++) {
    const uint32_t at = nextAnnounceAt(interval, now, r * 2654435761u);
    const uint32_t delay = at - now;
    total += delay;
    if (delay < lo) lo = delay;
    if (delay > hi) hi = delay;
  }
  const uint32_t mean = (uint32_t)(total / 20000);
  // Within a percent of the configured interval, from either side.
  TEST_ASSERT_UINT32_WITHIN(base / 100, base, mean);
  TEST_ASSERT_TRUE(lo < base);                      // some land early
  TEST_ASSERT_TRUE(hi > base);                      // and some late
}

// The spread stays a tenth of the interval — the decorrelation the jitter is
// there for is unchanged, only its centre moved.
static void test_the_spread_is_a_tenth_of_the_interval() {
  const uint16_t interval = 60;
  const uint32_t now = 0, base = 60000, spread = base / 10;
  for (uint32_t r = 0; r < 5000; r++) {
    const uint32_t delay = nextAnnounceAt(interval, now, r * 40503u) - now;
    TEST_ASSERT_TRUE(delay >= base - spread / 2);
    TEST_ASSERT_TRUE(delay <= base + spread / 2 + 1);
  }
}

// The longest interval the settings allow, where a one-sided jitter cost over
// an hour, and the shortest, where the spread is small enough to round away.
static void test_the_extremes_of_the_permitted_range_behave() {
  const uint32_t now = 12345;
  for (uint32_t r = 0; r < 2000; r++) {
    const uint32_t twelveHours = nextAnnounceAt(43200, now, r * 2246822519u) - now;
    TEST_ASSERT_TRUE(twelveHours >= 43200000u - 43200000u / 20 - 1);
    TEST_ASSERT_TRUE(twelveHours <= 43200000u + 43200000u / 20 + 1);
    const uint32_t oneMinute = nextAnnounceAt(60, now, r * 668265263u) - now;
    TEST_ASSERT_TRUE(oneMinute >= 57000 && oneMinute <= 63000);
  }
}

// millis() wraps every 49 days and the deadline is compared as a signed
// difference, so a booking made just before the wrap has to still be in the
// future afterwards.
static void test_a_deadline_booked_across_the_wrap_is_still_ahead() {
  const uint32_t nearWrap = 0xFFFFF000u;            // ~4 seconds before it wraps
  const uint32_t at = nextAnnounceAt(60, nearWrap, 0);
  TEST_ASSERT_TRUE(at < nearWrap);                  // it wrapped, as it must
  TEST_ASSERT_TRUE((int32_t)(nearWrap - at) < 0);   // and still reads as ahead
}

// Lowering the interval has to bring a booked announce forward, or a node set
// to twelve hours and dropped to one minute says nothing for half a day.
static void test_lowering_the_interval_pulls_a_booking_forward() {
  const uint32_t now = 500000;
  const uint32_t booked = now + 43200000u;          // twelve hours out
  const uint32_t pulled = clampAnnounceTo(booked, 60, now);
  TEST_ASSERT_EQUAL_UINT32(now + 60000, pulled);
}

// Raising it, or leaving it alone, must not push an imminent announce away.
static void test_a_booking_already_sooner_than_the_interval_is_left_alone() {
  const uint32_t now = 500000;
  const uint32_t soon = now + 5000;
  TEST_ASSERT_EQUAL_UINT32(soon, clampAnnounceTo(soon, 60, now));
  TEST_ASSERT_EQUAL_UINT32(soon, clampAnnounceTo(soon, 43200, now));
}

// The clamp is compared as a signed difference too, so it must not drag a
// booking that has wrapped back to the far side of the clock.
static void test_the_clamp_survives_the_wrap() {
  const uint32_t now = 0xFFFFF000u;
  const uint32_t booked = now + 60000;              // wrapped
  TEST_ASSERT_EQUAL_UINT32(booked, clampAnnounceTo(booked, 60, now));
}

// --- which announces earn a row -------------------------------------------
//
// A RetiMesh node announces itself under more than one aspect, and each one
// arrives as its own destination with its own hash. Filing all of them fills
// the table with rows describing one node several times over; the rule is
// which of them tells us something.

static void test_the_messaging_aspect_is_kept() {
  // The address a person sends to. Without this row there is no peer to talk
  // to and the whole list is decoration.
  TEST_ASSERT_TRUE(Rns::worthRemembering("lxmf.delivery"));
}

static void test_the_page_aspect_is_not_kept() {
  // It carries a name this node already has from the messaging announce and
  // nothing else. Ours is still announced so a NomadNet client can find the
  // page; that is a reason to advertise, not a reason to file everybody
  // else's.
  TEST_ASSERT_FALSE(Rns::worthRemembering("nomadnetwork.node"));
}

static void test_an_unknown_aspect_is_kept() {
  // A peer running something this table has never met is the most interesting
  // row in the list, not the one to hide. An unrecognised name hash arrives
  // here as nullptr, and a bench with a non-RetiMesh peer on it proved the
  // case: one row, no aspect, and worth seeing.
  TEST_ASSERT_TRUE(Rns::worthRemembering(nullptr));
  TEST_ASSERT_TRUE(Rns::worthRemembering(""));
  TEST_ASSERT_TRUE(Rns::worthRemembering("something.else"));
}

static void test_the_other_known_aspects_are_kept() {
  // Only the page aspect is dropped. Nothing else in the table was measured
  // to be redundant, and a rule that quietly grew would hide peers.
  TEST_ASSERT_TRUE(Rns::worthRemembering("lxmf.propagation"));
  TEST_ASSERT_TRUE(Rns::worthRemembering("rnstransport.probe"));
  TEST_ASSERT_TRUE(Rns::worthRemembering("retimesh.node"));
}

static void test_a_node_that_still_announces_the_old_aspect_is_not_hidden() {
  // This node stopped announcing retimesh.node, but a mesh updates one node at
  // a time and the rest keep sending it for as long as they are unattended.
  // Dropping it on receipt as well would make those neighbours disappear from
  // a list whose job is to show what is out there.
  TEST_ASSERT_TRUE(Rns::worthRemembering("retimesh.node"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_jitter_is_centred_on_the_interval);
  RUN_TEST(test_the_spread_is_a_tenth_of_the_interval);
  RUN_TEST(test_the_extremes_of_the_permitted_range_behave);
  RUN_TEST(test_a_deadline_booked_across_the_wrap_is_still_ahead);
  RUN_TEST(test_lowering_the_interval_pulls_a_booking_forward);
  RUN_TEST(test_a_booking_already_sooner_than_the_interval_is_left_alone);
  RUN_TEST(test_the_clamp_survives_the_wrap);
  RUN_TEST(test_the_messaging_aspect_is_kept);
  RUN_TEST(test_the_page_aspect_is_not_kept);
  RUN_TEST(test_an_unknown_aspect_is_kept);
  RUN_TEST(test_the_other_known_aspects_are_kept);
  RUN_TEST(test_a_node_that_still_announces_the_old_aspect_is_not_hidden);
  return UNITY_END();
}
