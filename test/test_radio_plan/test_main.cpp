// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// Region and radio-capability tests.
//
// The band plan and the chip descriptor are pure lookups, which is exactly why
// they are worth testing: a review found the regime table and the region table
// disagreeing about whether 870.000 MHz is European, and nothing in CI could
// have noticed. The invariants below are written so that a second copy of a
// table drifting from the first fails here rather than on a node.

#include <unity.h>
#include "Airtime.cpp"
#include "RadioCaps.cpp"
#include "Mdns.h"

// --- regions and regimes must agree, at the edges as well as the middle -----

// The bug this exists to catch: regimeFor() carried its own copy of the band
// edges, and the two disagreed at exactly 870.0 MHz — accepted as a European
// channel by the validator, reported as "outside every band plan" by the log,
// while a duty cycle was quietly being enforced.
static void test_every_region_agrees_with_the_regime_lookup() {
  size_t n = 0;
  const Airtime::RegionInfo* regions = Airtime::regions(n);
  for (size_t i = 0; i < n; i++) {
    const Airtime::RegionInfo& r = regions[i];
    if (r.id == Airtime::Region::Custom) continue;      // deliberately has none
    const float mid = (r.lowMhz + r.highMhz) / 2.0f;
    TEST_ASSERT_EQUAL_MESSAGE((int)r.regime, (int)Airtime::regimeFor(r.lowMhz),
                              "regime disagrees at the bottom edge");
    TEST_ASSERT_EQUAL_MESSAGE((int)r.regime, (int)Airtime::regimeFor(mid),
                              "regime disagrees mid-band");
    TEST_ASSERT_EQUAL_MESSAGE((int)r.regime, (int)Airtime::regimeFor(r.highMhz),
                              "regime disagrees at the top edge");
  }
}

// A frequency the validator will accept for a region must map back to it.
static void test_region_lookup_round_trips_at_the_edges() {
  size_t n = 0;
  const Airtime::RegionInfo* regions = Airtime::regions(n);
  for (size_t i = 0; i < n; i++) {
    const Airtime::RegionInfo& r = regions[i];
    if (r.id == Airtime::Region::Custom) continue;
    TEST_ASSERT_EQUAL((int)r.id, (int)Airtime::regionForFreq(r.lowMhz)->id);
    TEST_ASSERT_EQUAL((int)r.id, (int)Airtime::regionForFreq(r.highMhz)->id);
    TEST_ASSERT_EQUAL_STRING(r.key, Airtime::regionByKey(r.key)->key);
    TEST_ASSERT_EQUAL((int)r.id, (int)Airtime::regionById(r.id)->id);
  }
}

static void test_regions_do_not_overlap() {
  size_t n = 0;
  const Airtime::RegionInfo* regions = Airtime::regions(n);
  for (size_t i = 0; i < n; i++) {
    if (regions[i].id == Airtime::Region::Custom) continue;
    for (size_t j = i + 1; j < n; j++) {
      if (regions[j].id == Airtime::Region::Custom) continue;
      const bool overlap = regions[i].lowMhz <= regions[j].highMhz &&
                           regions[j].lowMhz <= regions[i].highMhz;
      TEST_ASSERT_FALSE_MESSAGE(overlap, "two regions claim the same frequency");
    }
  }
}

static void test_frequencies_outside_every_band_fall_to_custom() {
  TEST_ASSERT_EQUAL((int)Airtime::Region::Custom, (int)Airtime::regionForFreq(433.0f)->id);
  TEST_ASSERT_EQUAL((int)Airtime::Region::Custom, (int)Airtime::regionForFreq(1000.0f)->id);
  TEST_ASSERT_EQUAL((int)Airtime::Regime::None,   (int)Airtime::regimeFor(433.0f));
}

// Custom must not carry a channel of its own: it is offered on every radio,
// and a fixed 869.525 MHz default is unusable on a 2.4 GHz one. Zero means
// "no opinion, derive it from the chip".
static void test_custom_region_has_no_opinion_about_the_channel() {
  const Airtime::RegionInfo* c = Airtime::regionById(Airtime::Region::Custom);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c->defaultMhz);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c->defaultBwKhz);
}

// Each region's suggested channel has to sit inside the region.
static void test_region_defaults_are_inside_their_own_band() {
  size_t n = 0;
  const Airtime::RegionInfo* regions = Airtime::regions(n);
  for (size_t i = 0; i < n; i++) {
    const Airtime::RegionInfo& r = regions[i];
    if (r.defaultMhz == 0.0f) continue;               // no opinion, see above
    TEST_ASSERT_TRUE_MESSAGE(r.defaultMhz >= r.lowMhz && r.defaultMhz <= r.highMhz,
                             "a region suggests a channel outside itself");
  }
}

// --- what each regime actually caps ----------------------------------------

static void test_only_the_us_band_is_dwell_limited() {
  TEST_ASSERT_EQUAL_UINT32(0,   Airtime::maxDwellMs(Airtime::Regime::EuSrd868));
  TEST_ASSERT_EQUAL_UINT32(400, Airtime::maxDwellMs(Airtime::Regime::UsIsm915));
  TEST_ASSERT_EQUAL_UINT32(0,   Airtime::maxDwellMs(Airtime::Regime::Ism2400));
  TEST_ASSERT_EQUAL_UINT32(0,   Airtime::maxDwellMs(Airtime::Regime::None));

  TEST_ASSERT_EQUAL_FLOAT(500.0f, Airtime::dtsMinBandwidthKhz(Airtime::Regime::UsIsm915));
  TEST_ASSERT_EQUAL_FLOAT(0.0f,   Airtime::dtsMinBandwidthKhz(Airtime::Regime::EuSrd868));
}

// Outside the EU plan there is no hourly allowance to look up, so a node there
// is limited only by a cap the operator sets.
static void test_no_hourly_budget_outside_the_eu_plan() {
  TEST_ASSERT_EQUAL_UINT16(0, Airtime::effectiveBasisPoints(915.0f,  500.0f, 0));
  TEST_ASSERT_EQUAL_UINT16(0, Airtime::effectiveBasisPoints(2445.0f, 812.5f, 0));
  // ... and a manual cap still bites where the plan does not.
  TEST_ASSERT_EQUAL_UINT16(200, Airtime::effectiveBasisPoints(915.0f, 500.0f, 2));
}

// The US dwell ceiling is the whole point of that regime, so the maths that
// decides whether a frame fits has to be exercised, not just the constant.
static void test_a_full_frame_at_sf12_cannot_fit_the_us_dwell_limit() {
  Airtime a;
  Airtime::Params p; p.sf = 12; p.bwKhz = 125.0f; p.cr = 5; p.preambleSyms = 18; p.crcOn = true;
  a.configure(p);
  const float worst = a.timeOnAirMs(254);
  TEST_ASSERT_TRUE_MESSAGE(worst > (float)Airtime::maxDwellMs(Airtime::Regime::UsIsm915),
                           "SF12/125k should be well past 400 ms — the warning depends on it");

  Airtime b;
  Airtime::Params q; q.sf = 8; q.bwKhz = 500.0f; q.cr = 5; q.preambleSyms = 18; q.crcOn = true;
  b.configure(q);
  TEST_ASSERT_TRUE_MESSAGE(b.timeOnAirMs(254) < (float)Airtime::maxDwellMs(Airtime::Regime::UsIsm915),
                           "SF8/500k should fit comfortably");
}

// --- radio capabilities -----------------------------------------------------

// SF6 puts an SX127x into implicit-header mode, which breaks the variable
// length framing this firmware relies on. The validator takes its lower bound
// straight from here, so this is where that has to be prevented.
static void test_no_radio_offers_a_spreading_factor_that_breaks_the_framing() {
  const RadioCaps::Caps* all[] = { &RadioCaps::kSX1276, &RadioCaps::kSX1262,
                                   &RadioCaps::kSX1280, &RadioCaps::kUnknown };
  for (const RadioCaps::Caps* c : all) {
    TEST_ASSERT_TRUE_MESSAGE(c->sfMin >= 7, "SF6 forces implicit-header mode on the SX127x");
    TEST_ASSERT_TRUE(c->sfMax >= c->sfMin);
    TEST_ASSERT_TRUE(c->txMaxDbm > c->txMinDbm);
    TEST_ASSERT_TRUE(c->freqMaxMhz > c->freqMinMhz);
  }
}

// The validator must not accept a bandwidth the driver will refuse. RadioLib
// matches to within 0.001 kHz; anything looser lets a value through that is
// saved to NVS and then fails to apply, leaving the radio offline on reboot.
static void test_bandwidths_are_matched_as_tightly_as_the_driver_does() {
  TEST_ASSERT_TRUE(RadioCaps::bandwidthSupported(RadioCaps::kSX1280, 203.125f));
  TEST_ASSERT_TRUE(RadioCaps::bandwidthSupported(RadioCaps::kSX1280, 812.5f));
  TEST_ASSERT_FALSE_MESSAGE(RadioCaps::bandwidthSupported(RadioCaps::kSX1280, 203.1f),
                            "203.1 is not 203.125 and RadioLib will reject it");
  TEST_ASSERT_FALSE(RadioCaps::bandwidthSupported(RadioCaps::kSX1280, 125.0f));

  TEST_ASSERT_TRUE(RadioCaps::bandwidthSupported(RadioCaps::kSX1276, 125.0f));
  TEST_ASSERT_FALSE(RadioCaps::bandwidthSupported(RadioCaps::kSX1276, 812.5f));
  TEST_ASSERT_FALSE(RadioCaps::bandwidthSupported(RadioCaps::kSX1276, 125.06f));
}

// A 2.4 GHz radio must not be able to claim a sub-GHz band, and vice versa:
// this is what stops the settings page offering a region the chip cannot tune.
static void test_radio_ranges_and_region_bands_only_overlap_where_they_should() {
  const Airtime::RegionInfo* eu   = Airtime::regionById(Airtime::Region::Eu868);
  const Airtime::RegionInfo* us   = Airtime::regionById(Airtime::Region::Us915);
  const Airtime::RegionInfo* ism  = Airtime::regionById(Airtime::Region::Ism2400);
  auto reaches = [](const RadioCaps::Caps& c, const Airtime::RegionInfo* r) {
    return r->highMhz >= c.freqMinMhz && r->lowMhz <= c.freqMaxMhz;
  };
  TEST_ASSERT_TRUE (reaches(RadioCaps::kSX1276, eu));
  TEST_ASSERT_TRUE (reaches(RadioCaps::kSX1276, us));
  TEST_ASSERT_FALSE(reaches(RadioCaps::kSX1276, ism));
  TEST_ASSERT_TRUE (reaches(RadioCaps::kSX1262, us));
  TEST_ASSERT_FALSE(reaches(RadioCaps::kSX1262, ism));
  TEST_ASSERT_TRUE (reaches(RadioCaps::kSX1280, ism));
  TEST_ASSERT_FALSE(reaches(RadioCaps::kSX1280, eu));
  TEST_ASSERT_FALSE(reaches(RadioCaps::kSX1280, us));
}

// Flashing a 2.4 GHz image over a board that ran a sub-GHz one leaves NVS
// holding a channel none of which this chip accepts. Every part of it has to be
// recognised as unusable, not just the frequency: the two bandwidth lists share
// no value at all, so a frequency-only check still hands begin() a bandwidth it
// rejects, and the probe then reports a wiring fault for a settings problem.
static void test_a_sub_ghz_channel_is_recognised_as_unusable_on_a_2400_radio() {
  TEST_ASSERT_FALSE(RadioCaps::channelUsable(RadioCaps::kSX1280, 869.525f, 125.0f, 8));
  TEST_ASSERT_FALSE_MESSAGE(RadioCaps::channelUsable(RadioCaps::kSX1280, 2445.0f, 125.0f, 8),
                            "right band, but 125 kHz is not a bandwidth this chip has");
  TEST_ASSERT_FALSE(RadioCaps::channelUsable(RadioCaps::kSX1280, 869.525f, 812.5f, 8));
  TEST_ASSERT_TRUE (RadioCaps::channelUsable(RadioCaps::kSX1280, 2445.0f, 812.5f, 8));
  // ... and the reverse, for an image flashed the other way round
  TEST_ASSERT_FALSE(RadioCaps::channelUsable(RadioCaps::kSX1276, 2445.0f, 812.5f, 8));
  TEST_ASSERT_TRUE (RadioCaps::channelUsable(RadioCaps::kSX1276, 869.525f, 125.0f, 8));
  // A spreading factor outside the range counts too
  TEST_ASSERT_FALSE(RadioCaps::channelUsable(RadioCaps::kSX1276, 869.525f, 125.0f, 6));
}

// With no radio detected the validator has nothing to protect, so it must not
// reject the settings an operator is entering while they fix the wiring —
// including 2.4 GHz ones, which a sub-GHz-only list would have refused.
static void test_the_unknown_radio_accepts_any_bandwidth_either_family_offers() {
  for (const float* b = RadioCaps::kBwSubGhz; *b != 0.0f; b++)
    TEST_ASSERT_TRUE(RadioCaps::bandwidthSupported(RadioCaps::kUnknown, *b));
  for (const float* b = RadioCaps::kBwSx128x; *b != 0.0f; b++)
    TEST_ASSERT_TRUE_MESSAGE(RadioCaps::bandwidthSupported(RadioCaps::kUnknown, *b),
                             "a failed SX1280 probe must not lock the operator out of 2.4 GHz");
  TEST_ASSERT_FALSE(RadioCaps::bandwidthSupported(RadioCaps::kUnknown, 300.0f));
}

// The region the operator chose decides the rules. A channel at 868 MHz under
// "custom" was being told no plan applied while the European duty cycle went on
// being enforced underneath it.
static void test_the_region_decides_the_budget_not_the_frequency() {
  const uint16_t eu = Airtime::effectiveBasisPoints(Airtime::Regime::EuSrd868, 869.525f, 125.0f, 0);
  TEST_ASSERT_TRUE_MESSAGE(eu > 0, "the EU plan still supplies a budget of its own");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0,
    Airtime::effectiveBasisPoints(Airtime::Regime::None, 869.525f, 125.0f, 0),
    "custom at a European frequency must not inherit the European allowance");
  TEST_ASSERT_EQUAL_UINT16(0,
    Airtime::effectiveBasisPoints(Airtime::Regime::UsIsm915, 869.525f, 125.0f, 0));
  // A manual cap is the only budget outside the EU plan, and still applies
  TEST_ASSERT_EQUAL_UINT16(500,
    Airtime::effectiveBasisPoints(Airtime::Regime::None, 869.525f, 125.0f, 5));
}

// Everything that needs to know which rules apply must get the same answer.
// Three call sites derived this separately and one was missed, so the API said
// the EU plan governed a node whose radio had already stopped applying it.
static void test_the_stored_region_wins_and_the_frequency_is_only_a_fallback() {
  // Stored region is authoritative even where the frequency says otherwise
  TEST_ASSERT_EQUAL((int)Airtime::Regime::None,
                    (int)Airtime::regionFor("custom", 869.525f)->regime);
  TEST_ASSERT_EQUAL((int)Airtime::Regime::EuSrd868,
                    (int)Airtime::regionFor("eu868", 869.525f)->regime);
  // An empty or unknown key is a configuration written before regions existed,
  // and the frequency is what it has to be recovered from
  TEST_ASSERT_EQUAL((int)Airtime::Regime::EuSrd868,
                    (int)Airtime::regionFor("", 869.525f)->regime);
  TEST_ASSERT_EQUAL((int)Airtime::Regime::UsIsm915,
                    (int)Airtime::regionFor(nullptr, 915.0f)->regime);
  TEST_ASSERT_EQUAL((int)Airtime::Regime::Ism2400,
                    (int)Airtime::regionFor("nonsense", 2445.0f)->regime);
  // Never null, whatever it is handed
  TEST_ASSERT_NOT_NULL(Airtime::regionFor(nullptr, 1.0f));
}

// Every node used to answer to the same "retimesh.local", so a second one on
// the network was renamed unpredictably by conflict resolution. The name now
// comes from the access-point name, which means it has to survive whatever an
// operator has typed there.
static void test_a_node_name_becomes_a_legal_dns_label() {
  char out[33];

  Mdns::label("retimesh-D96308", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("retimesh-d96308", out);

  // Spaces and punctuation collapse to single hyphens
  Mdns::label("Shed roof #2", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("shed-roof-2", out);
  Mdns::label("a__b  c", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("a-b-c", out);

  // A label may not begin or end with a hyphen
  Mdns::label("  edge  ", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("edge", out);
  Mdns::label("---", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("retimesh", out, "nothing usable survived, so the fallback stands");

  // Nothing usable at all still leaves a registrable name
  Mdns::label("", out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("retimesh", out);
  Mdns::label(nullptr, out, sizeof(out), "retimesh");
  TEST_ASSERT_EQUAL_STRING("retimesh", out);

  // And it never runs past the buffer it was given
  char small[6];
  Mdns::label("abcdefghij", small, sizeof(small), "x");
  TEST_ASSERT_EQUAL_STRING("abcde", small);
  TEST_ASSERT_EQUAL_UINT(5, strlen(small));
}

static void test_bandwidth_list_renders_for_an_error_message() {
  char buf[96];
  RadioCaps::bandwidthList(RadioCaps::kSX1280, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("203.125, 406.25, 812.5, 1625", buf);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_every_region_agrees_with_the_regime_lookup);
  RUN_TEST(test_region_lookup_round_trips_at_the_edges);
  RUN_TEST(test_regions_do_not_overlap);
  RUN_TEST(test_frequencies_outside_every_band_fall_to_custom);
  RUN_TEST(test_custom_region_has_no_opinion_about_the_channel);
  RUN_TEST(test_region_defaults_are_inside_their_own_band);
  RUN_TEST(test_only_the_us_band_is_dwell_limited);
  RUN_TEST(test_no_hourly_budget_outside_the_eu_plan);
  RUN_TEST(test_a_full_frame_at_sf12_cannot_fit_the_us_dwell_limit);
  RUN_TEST(test_no_radio_offers_a_spreading_factor_that_breaks_the_framing);
  RUN_TEST(test_bandwidths_are_matched_as_tightly_as_the_driver_does);
  RUN_TEST(test_radio_ranges_and_region_bands_only_overlap_where_they_should);
  RUN_TEST(test_a_sub_ghz_channel_is_recognised_as_unusable_on_a_2400_radio);
  RUN_TEST(test_the_unknown_radio_accepts_any_bandwidth_either_family_offers);
  RUN_TEST(test_the_region_decides_the_budget_not_the_frequency);
  RUN_TEST(test_the_stored_region_wins_and_the_frequency_is_only_a_fallback);
  RUN_TEST(test_a_node_name_becomes_a_legal_dns_label);
  RUN_TEST(test_bandwidth_list_renders_for_an_error_message);
  return UNITY_END();
}
