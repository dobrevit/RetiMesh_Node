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

static void test_the_wifi_tx_ceiling_is_two_to_twenty_dbm() {
  // The driver's own window in quarter-dBm is [8,84]; 2-20 dBm is the slice
  // offered, and the refusal names it. The value is what may be asked for —
  // the driver then quantizes down to its own steps, which the read-back
  // reports, so the bound holds the request rather than the result.
  WifiSettings w;
  char err[160] = "";
  w.txPowerDbm = 1;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "2-20"));
  w.txPowerDbm = 2;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.txPowerDbm = 20;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.txPowerDbm = 21;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "2-20"));
}

static void test_the_listen_interval_counts_beacons_one_to_sixteen() {
  // Units are AP beacon intervals, and zero is not "off" — the driver reads
  // 0 as its default, which is a value that lies about itself in a settings
  // dump. The refusal names the bound.
  WifiSettings w;
  char err[160] = "";
  w.staListenInterval = 0;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "1-16"));
  w.staListenInterval = 1;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.staListenInterval = 16;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.staListenInterval = 17;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "1-16"));
}

static void test_the_wifi_restart_split_is_pinned_per_field() {
  // The live/restart split lives in wifiChangeNeedsRestart and nowhere else.
  // Every field is asserted one at a time, so a WifiSettings member added
  // without being classified shows up as a missing line here rather than as
  // a setting that silently never restarts (the predicate enumerates the
  // restart fields, so omission means live).
  WifiSettings a, b;
  TEST_ASSERT_FALSE_MESSAGE(wifiChangeNeedsRestart(a, b), "no change must not restart");

  b = a; b.txPowerDbm = (int8_t)(a.txPowerDbm == 7 ? 8 : 7);
  TEST_ASSERT_FALSE_MESSAGE(wifiChangeNeedsRestart(a, b), "tx power applies live");
  b = a; b.staListenInterval = (uint8_t)(a.staListenInterval == 9 ? 10 : 9);
  TEST_ASSERT_FALSE_MESSAGE(wifiChangeNeedsRestart(a, b), "listen interval applies live");
  b = a; b.apIdleOff = !a.apIdleOff;
  TEST_ASSERT_FALSE_MESSAGE(wifiChangeNeedsRestart(a, b), "the AP idle switch arms a timer, live");
  b = a; b.apIdleMinutes = (uint16_t)(a.apIdleMinutes == 30 ? 31 : 30);
  TEST_ASSERT_FALSE_MESSAGE(wifiChangeNeedsRestart(a, b), "the AP idle window arms a timer, live");

  b = a; strlcpy(b.ssid, "another-name", sizeof(b.ssid));
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "ssid rebuilds the AP");
  b = a; strlcpy(b.password, "anotherpass", sizeof(b.password));
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "password rebuilds the AP");
  b = a; b.security = (a.security == ApSecurity::Open) ? ApSecurity::WPA2 : ApSecurity::Open;
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "security rebuilds the AP");
  b = a; b.channel = (uint8_t)(a.channel == 6 ? 7 : 6);
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "channel rebuilds the AP");
  b = a; b.maxStations = (uint8_t)(a.maxStations == 4 ? 5 : 4);
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "max stations rebuilds the AP");
  b = a; b.hidden = !a.hidden;
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "hidden rebuilds the AP");
  b = a; strlcpy(b.staSsid, "another-lan", sizeof(b.staSsid));
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "station ssid rebuilds the join");
  b = a; strlcpy(b.staPassword, "another-lan-pass", sizeof(b.staPassword));
  TEST_ASSERT_TRUE_MESSAGE(wifiChangeNeedsRestart(a, b), "station password rebuilds the join");
}

static void test_the_ap_idle_window_counts_minutes_one_to_a_day() {
  // 1-1440: a day is the most an idle window can mean, and zero is not
  // "off" — wifi.ap_idle_off is. Held whether or not the switch is on, so a
  // stored value cannot walk in the moment the feature is enabled.
  WifiSettings w;
  char err[160] = "";
  w.apIdleOff = false;                       // the bound holds even switched off
  w.apIdleMinutes = 0;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "1-1440"));
  w.apIdleMinutes = 1;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.apIdleMinutes = 1440;
  TEST_ASSERT_TRUE_MESSAGE(validateWifi(w, err, sizeof(err)), err);
  w.apIdleMinutes = 1441;
  TEST_ASSERT_FALSE(validateWifi(w, err, sizeof(err)));
  TEST_ASSERT_NOT_NULL(strstr(err, "1-1440"));
}

static void test_the_ap_idle_defaults_ship_off_and_at_ten_minutes() {
  // Default OFF is a decision, not an accident: the AP is usually the only
  // management path on an unattended node, and the operator opts in per
  // node. Changing either default should have to change this test.
  WifiSettings w;
  TEST_ASSERT_FALSE_MESSAGE(w.apIdleOff, "AP idle auto-off must ship OFF");
  TEST_ASSERT_EQUAL_UINT16(10, w.apIdleMinutes);
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
  RUN_TEST(test_the_wifi_tx_ceiling_is_two_to_twenty_dbm);
  RUN_TEST(test_the_listen_interval_counts_beacons_one_to_sixteen);
  RUN_TEST(test_the_wifi_restart_split_is_pinned_per_field);
  RUN_TEST(test_the_ap_idle_window_counts_minutes_one_to_a_day);
  RUN_TEST(test_the_ap_idle_defaults_ship_off_and_at_ten_minutes);
  RUN_TEST(test_an_ssid_is_judged_before_it_is_truncated);
  return UNITY_END();
}
