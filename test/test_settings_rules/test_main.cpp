// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// Settings rules: what a value may be, held to the same vectors the firmware
// uses. These bounds used to live inside the HTTP handlers, one copy each,
// and the console's copies had already drifted before anyone ran them — an
// announce cap of 0 that HTTP refused, an admin password bound of 8-32
// against the API's 4-32. A rule with two homes is a rule with two answers,
// and this suite is what makes the single home hold.
#include <unity.h>
#include "Airtime.cpp"
#include "RadioCaps.cpp"
#include "SettingsRules.h"

using namespace SettingsRules;

// The transceiver a rule is asked about is a parameter, not an assumption:
// an SX1262 tunes sub-GHz and an SX1280 does not, and a bound that ignored
// that would refuse a legal channel on one of them.
static const RadioCaps::Caps& sub() { return RadioCaps::kSX1262; }

static RadioSettings good() {
  RadioSettings r;
  strlcpy(r.region, "eu868", sizeof(r.region));
  r.freqMhz = 869.525f; r.bwKhz = 125.0f; r.sf = 8; r.cr = 5; r.txDbm = 7;
  r.preamble = 18; r.beaconInterval = 0; r.announceInterval = 600; r.dutyCyclePct = 0;
  r.callsign[0] = '\0';
  return r;
}

static void test_a_sound_radio_setting_passes() {
  char err[160] = "";
  TEST_ASSERT_TRUE_MESSAGE(validateRadio(good(), sub(), 22, err, sizeof(err)), err);
}

static void test_a_frequency_outside_the_region_is_refused_by_name() {
  RadioSettings r = good();
  r.freqMhz = 915.0f;                       // legal in the US, not in eu868
  char err[160] = "";
  TEST_ASSERT_FALSE(validateRadio(r, sub(), 22, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "frequency must be"));
}

static void test_a_not_a_number_frequency_cannot_pass() {
  // Every comparison against a NaN is false, so a NaN slips through a bounds
  // check written as "< low || > high" and is stored as the channel. The
  // parser refuses it before it gets here; this holds the rule to it as well.
  RadioSettings r = good();
  r.freqMhz = NAN;
  char err[160] = "";
  TEST_ASSERT_FALSE_MESSAGE(validateRadio(r, sub(), 22, err, sizeof(err)),
                            "a NaN frequency must not validate");
}

static void test_the_fitted_radio_sets_the_bounds_not_a_constant() {
  RadioSettings r = good();
  r.sf = 6;                                  // below the SX1262's floor
  char err[160] = "";
  TEST_ASSERT_FALSE(validateRadio(r, sub(), 22, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "SX1262"));

  r = good(); r.txDbm = 30;                  // above what the driver reports
  TEST_ASSERT_FALSE(validateRadio(r, sub(), 22, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "tx power must be"));
}

static void test_a_callsign_is_judged_before_it_is_truncated() {
  // The field is char[33], so a length check on the stored value can only ever
  // see a truncated callsign and pass. The rule takes the raw text.
  char err[160] = "";
  char long_one[64];
  memset(long_one, 'A', sizeof(long_one)); long_one[40] = '\0';
  TEST_ASSERT_FALSE(validateCallsign(long_one, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "at most 32"));

  TEST_ASSERT_FALSE(validateCallsign("has space", err, sizeof(err)));
  TEST_ASSERT_TRUE(validateCallsign("M0ABC-1", err, sizeof(err)));
  TEST_ASSERT_TRUE(validateCallsign("", err, sizeof(err)));
}

static void test_the_admin_password_bound_is_the_apis() {
  // 4-32, which is what POST /api/settings/admin enforces. The console once
  // said 8-32, so a password set over HTTP could not be retyped over the cable.
  char err[160] = "";
  TEST_ASSERT_FALSE(validateAdminPassword("abc", err, sizeof(err)));
  TEST_ASSERT_TRUE(validateAdminPassword("abcd", err, sizeof(err)));
  TEST_ASSERT_TRUE(validateAdminPassword("abcdefgh", err, sizeof(err)));
  char too_long[64];
  memset(too_long, 'x', sizeof(too_long)); too_long[33] = '\0';
  TEST_ASSERT_FALSE(validateAdminPassword(too_long, err, sizeof(err)));
}

static void test_the_announce_cap_floor_is_one_not_zero() {
  // 0 % would be a node that never announces, which is what the switch is
  // for. The console accepted 0 while HTTP refused it.
  TransportSettings t;
  char err[160] = "";
  t.announceCap = 0;
  TEST_ASSERT_FALSE(validateTransport(t, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "1-100"));
  t.announceCap = 1;
  TEST_ASSERT_TRUE_MESSAGE(validateTransport(t, err, sizeof(err)), err);
  t.announceCap = 101;
  TEST_ASSERT_FALSE(validateTransport(t, err, sizeof(err)));
}

static void test_transport_modes_are_rnsds_one_to_five() {
  TransportSettings t;
  char err[160] = "";
  for (uint8_t m = 1; m <= 5; m++) {
    t.loraMode = t.wifiMode = t.autoMode = m;
    TEST_ASSERT_TRUE_MESSAGE(validateTransport(t, err, sizeof(err)), err);
  }
  t.loraMode = 0; TEST_ASSERT_FALSE(validateTransport(t, err, sizeof(err)));
  t.loraMode = 1; t.wifiMode = 6; TEST_ASSERT_FALSE(validateTransport(t, err, sizeof(err)));
}

static void test_a_secured_network_needs_a_password_and_the_lengths_are_wifis() {
  WifiSettings w;
  char err[160] = "";
  w.security = ApSecurity::WPA2;
  strlcpy(w.password, "short", sizeof(w.password));
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));

  strlcpy(w.password, "longenough", sizeof(w.password));
  w.channel = 6; w.maxStations = 8;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);

  w.channel = 14;                            // 1-13 here
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  w.channel = 6; w.maxStations = 11;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
}

static void test_an_ssid_is_judged_before_it_is_truncated() {
  char err[160] = "";
  char long_one[64];
  memset(long_one, 'S', sizeof(long_one)); long_one[40] = '\0';
  TEST_ASSERT_FALSE(validateSsid(long_one, false, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "ssid must be"));
  TEST_ASSERT_FALSE(validateSsid(long_one, true, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "station ssid"));
  TEST_ASSERT_TRUE(validateSsid("", false, err, sizeof(err)));      // empty = derive one
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_sound_radio_setting_passes);
  RUN_TEST(test_a_frequency_outside_the_region_is_refused_by_name);
  RUN_TEST(test_a_not_a_number_frequency_cannot_pass);
  RUN_TEST(test_the_fitted_radio_sets_the_bounds_not_a_constant);
  RUN_TEST(test_a_callsign_is_judged_before_it_is_truncated);
  RUN_TEST(test_the_admin_password_bound_is_the_apis);
  RUN_TEST(test_the_announce_cap_floor_is_one_not_zero);
  RUN_TEST(test_transport_modes_are_rnsds_one_to_five);
  RUN_TEST(test_a_secured_network_needs_a_password_and_the_lengths_are_wifis);
  RUN_TEST(test_an_ssid_is_judged_before_it_is_truncated);
  return UNITY_END();
}
