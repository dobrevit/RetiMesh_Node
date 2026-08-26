// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// Airtime: time-on-air maths, the rolling hour, the duty-cycle limiter and
// the CSMA parameters. Runs on the host: pio test -e native
#include <unity.h>
#include "Airtime.cpp"

static Airtime make(uint8_t sf = 8, float bw = 125.0f, uint8_t cr = 5, uint16_t pre = 8) {
  Airtime a;
  Airtime::Params p; p.sf = sf; p.bwKhz = bw; p.cr = cr; p.preambleSyms = pre; p.crcOn = true;
  a.configure(p);
  return a;
}

// Explicit header, CRC on, 8-symbol preamble, 10-byte payload — the
// configuration every published LoRa airtime calculator uses as its example.
void test_time_on_air_matches_datasheet() {
  Airtime a = make(7, 125.0f, 5, 8);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 41.2f, a.timeOnAirMs(10));     // SF7  BW125 CR4/5
  Airtime b = make(9, 125.0f, 5, 8);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 144.4f, b.timeOnAirMs(10));    // SF9
  Airtime c = make(12, 125.0f, 5, 8);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 991.2f, c.timeOnAirMs(10));    // SF12, low-rate opt on

  // A full RNode frame on the node's default channel: SF8, BW125, CR4/5,
  // 18-symbol preamble, 255 bytes — 0.73 s, so a 1 % hourly budget is worth
  // about 49 of them.
  Airtime d = make(8, 125.0f, 5, 18);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 727.6f, d.timeOnAirMs(255));
}

void test_symbol_time_and_slot() {
  Airtime a = make(8, 125.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.048f, a.symbolTimeMs());
  TEST_ASSERT_EQUAL_UINT32(25, a.slotMs());            // 12 symbols = 24.6 ms
  TEST_ASSERT_EQUAL_UINT32(50, a.difsMs());
  Airtime fast = make(5, 500.0f);                      // tiny symbols -> clamped
  TEST_ASSERT_EQUAL_UINT32(Airtime::SLOT_MIN_MS, fast.slotMs());
  Airtime slow = make(12, 125.0f);                     // huge symbols -> clamped
  TEST_ASSERT_EQUAL_UINT32(Airtime::SLOT_MAX_MS, slow.slotMs());
}

void test_long_term_util_over_the_hour() {
  Airtime a = make();
  uint32_t now = 1000;
  // 36 s of airtime spread over the hour is 1 % of it.
  for (int i = 0; i < 60; i++) a.addTx(now + (uint32_t)i * Airtime::BIN_MS, 600.0f);
  uint32_t end = now + 59UL * Airtime::BIN_MS;
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.01f, a.longTermUtil(end));
}

void test_duty_cycle_lock_and_release() {
  Airtime a = make();
  uint32_t now = 0;
  TEST_ASSERT_FALSE(a.locked(now, 100));
  // 36 s in one bin is the whole 1 % hourly allowance.
  a.addTx(now, 36000.0f);
  TEST_ASSERT_TRUE(a.locked(now, 100));                // 100 bp = 1 %
  TEST_ASSERT_FALSE(a.locked(now, 1000));              // still fine in a 10 % band
  TEST_ASSERT_FALSE(a.locked(now, 0));                 // 0 = no limit
  TEST_ASSERT_TRUE(a.retryAfterS(now, 100) > 0);
  // An hour later that bin has rolled out of the window.
  uint32_t later = now + (uint32_t)Airtime::BINS * Airtime::BIN_MS;
  TEST_ASSERT_FALSE(a.locked(later, 100));
  TEST_ASSERT_EQUAL_UINT32(0, a.retryAfterS(later, 100));
}

void test_budget_used_reports_fraction_of_allowance() {
  Airtime a = make();
  a.addTx(0, 18000.0f);                                // half of the 1 % budget
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.5f, a.budgetUsed(0, 100));
}

// The allowance belongs to the sub-band, not to the operator's preference.
void test_band_lookup_covers_the_eu_plan() {
  // A narrow channel well inside each sub-band gets that sub-band's figure.
  TEST_ASSERT_EQUAL_UINT16(100,  Airtime::bandFor(868.100f, 125.0f)->basisPoints);   // 1 %
  TEST_ASSERT_EQUAL_UINT16(1000, Airtime::bandFor(869.500f, 125.0f)->basisPoints);   // 10 %
  TEST_ASSERT_EQUAL_UINT16(10,   Airtime::bandFor(868.800f, 125.0f)->basisPoints);   // 0.1 %
  TEST_ASSERT_EQUAL_UINT16(100,  Airtime::bandFor(865.500f, 125.0f)->basisPoints);   // 1 %
  TEST_ASSERT_EQUAL_UINT16(10,   Airtime::bandFor(864.000f, 125.0f)->basisPoints);   // 0.1 %
  TEST_ASSERT_EQUAL_UINT16(100,  Airtime::bandFor(869.850f, 125.0f)->basisPoints);   // 1 %

  // The ranges between the sub-bands are not allocated to this kind of device.
  // They are still in the table, carrying the strictest allowance, so that a
  // channel there is throttled rather than handed a free hand.
  const Airtime::Band* gap = Airtime::bandFor(868.650f, 125.0f);
  TEST_ASSERT_NOT_NULL(gap);
  TEST_ASSERT_FALSE(gap->allocated);
  TEST_ASSERT_EQUAL_UINT16(10, gap->basisPoints);

  // Outside the plan there is nothing to look up.
  TEST_ASSERT_NULL(Airtime::bandFor(915.000f, 125.0f));
  TEST_ASSERT_NULL(Airtime::bandFor(433.000f, 125.0f));
}

// A channel is not a point: 125 kHz centred on a boundary lands in both
// neighbours, and the stricter of the two is the one that applies.
void test_channel_straddling_a_boundary_takes_the_stricter_band() {
  // 868.6 is the top of the 1 % sub-band and the bottom of an unallocated gap.
  TEST_ASSERT_EQUAL_UINT16(10, Airtime::bandFor(868.600f, 125.0f)->basisPoints);
  // Far enough below it that the whole channel fits in the 1 % sub-band.
  TEST_ASSERT_EQUAL_UINT16(100, Airtime::bandFor(868.500f, 125.0f)->basisPoints);
  // The top of the 10 % sub-band spills into the gap above it.
  TEST_ASSERT_EQUAL_UINT16(10, Airtime::bandFor(869.650f, 125.0f)->basisPoints);
  // Narrower channel, same centre: now it fits, and keeps the 10 %.
  TEST_ASSERT_EQUAL_UINT16(1000, Airtime::bandFor(869.600f, 62.5f)->basisPoints);
}

void test_effective_limit_follows_the_band() {
  // No manual cap: the band decides, less the 5 % safety margin. Basis points
  // express that exactly for every figure in the plan except 0.1 %.
  TEST_ASSERT_EQUAL_UINT16(95,  Airtime::effectiveBasisPoints(868.100f, 125.0f, 0));  // 1 %  -> 0.95 %
  TEST_ASSERT_EQUAL_UINT16(950, Airtime::effectiveBasisPoints(869.500f, 125.0f, 0));  // 10 % -> 9.5 %
  TEST_ASSERT_EQUAL_UINT16(9,   Airtime::effectiveBasisPoints(868.800f, 125.0f, 0));  // 0.1 % -> 0.09 %

  // A manual cap only ever tightens things.
  TEST_ASSERT_EQUAL_UINT16(100, Airtime::effectiveBasisPoints(869.500f, 125.0f, 1));  // 1 % cap in a 10 % band
  TEST_ASSERT_EQUAL_UINT16(95,  Airtime::effectiveBasisPoints(868.100f, 125.0f, 5));  // 5 % cap cannot loosen 1 %

  // Outside the plan we cannot know the rule: the operator's setting stands,
  // and 0 there means no limiter at all.
  TEST_ASSERT_EQUAL_UINT16(200, Airtime::effectiveBasisPoints(915.000f, 125.0f, 2));
  TEST_ASSERT_EQUAL_UINT16(0,   Airtime::effectiveBasisPoints(915.000f, 125.0f, 0));
}

void test_bins_expire_rather_than_accumulate() {
  Airtime a = make();
  a.addTx(0, 10000.0f);
  // Two hours of silence: nothing of the old traffic may remain.
  uint32_t much_later = 2UL * Airtime::BINS * Airtime::BIN_MS;
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, a.longTermUtil(much_later));
}

void test_contention_window_widens_with_channel_use() {
  Airtime a = make();
  uint8_t lo = 0, hi = 0;
  a.contentionWindow(0.0f, lo, hi);                    // idle channel
  TEST_ASSERT_EQUAL_UINT8(1, a.cwBand(0.0f));
  TEST_ASSERT_EQUAL_UINT8(0, lo);
  TEST_ASSERT_EQUAL_UINT8(Airtime::CW_PER_BAND - 1, hi);

  TEST_ASSERT_EQUAL_UINT8(1, a.cwBand(0.05f));         // 5 % still band 1
  TEST_ASSERT_TRUE(a.cwBand(0.50f) >= 2);              // busy channel backs off further
  TEST_ASSERT_EQUAL_UINT8(Airtime::CW_BANDS, a.cwBand(0.95f));

  a.contentionWindow(0.95f, lo, hi);
  TEST_ASSERT_EQUAL_UINT8((Airtime::CW_BANDS - 1) * Airtime::CW_PER_BAND, lo);
  TEST_ASSERT_TRUE(hi > lo);
}

void test_short_term_util_uses_recent_bins_only() {
  Airtime a = make();
  a.addTx(0, 30000.0f);                                // half of one bin
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.25f, a.shortTermUtil(0));   // over two bins
  uint32_t twoBinsLater = 2 * Airtime::BIN_MS + 100;
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, a.shortTermUtil(twoBinsLater));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_time_on_air_matches_datasheet);
  RUN_TEST(test_symbol_time_and_slot);
  RUN_TEST(test_long_term_util_over_the_hour);
  RUN_TEST(test_duty_cycle_lock_and_release);
  RUN_TEST(test_budget_used_reports_fraction_of_allowance);
  RUN_TEST(test_band_lookup_covers_the_eu_plan);
  RUN_TEST(test_channel_straddling_a_boundary_takes_the_stricter_band);
  RUN_TEST(test_effective_limit_follows_the_band);
  RUN_TEST(test_bins_expire_rather_than_accumulate);
  RUN_TEST(test_contention_window_widens_with_channel_use);
  RUN_TEST(test_short_term_util_uses_recent_bins_only);
  return UNITY_END();
}
