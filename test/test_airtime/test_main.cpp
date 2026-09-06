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

// --- duty-cycled receive ----------------------------------------------------
// These pin the arithmetic RadioLib 7.7.1 does behind
// SX126x::startReceiveDutyCycleAuto, because the driver applies it silently:
// it returns success either way and simply arms a continuous receive when the
// sleep comes out too short. Without a test the difference between "the
// receiver is sleeping" and "the receiver is not" is invisible from here.

// The constants themselves, so a driver update that retunes any of them lands
// as a failing test rather than as a node that quietly stopped sleeping.
void test_the_driver_constants_are_the_ones_we_mirror() {
  TEST_ASSERT_EQUAL_UINT16(8,  Airtime::RX_DC_MIN_SYMBOLS_SF7);
  TEST_ASSERT_EQUAL_UINT16(12, Airtime::RX_DC_MIN_SYMBOLS_SF6);
  TEST_ASSERT_EQUAL_UINT32(1016, Airtime::RX_DC_TRANSITION_US);
  TEST_ASSERT_EQUAL_UINT32(5000, Airtime::RX_DC_TCXO_DELAY_US);
  // 1016 is what the driver compares against; 1000 is what it then subtracts.
  // Two literals in RadioLib, two constants here — if they are ever collapsed
  // into one the boundary moves by 16 us in whichever direction the fold went.
  TEST_ASSERT_EQUAL_UINT32(1000, Airtime::RX_DC_COMPENSATION_US);
  TEST_ASSERT_NOT_EQUAL_UINT32(Airtime::RX_DC_TRANSITION_US, Airtime::RX_DC_COMPENSATION_US);
  // SetRxDutyCycle carries the period in three bytes.
  TEST_ASSERT_EQUAL_UINT32(0x00FFFFFFUL, Airtime::RX_DC_PERIOD_RAW_MAX);
  // SF7 and up take the 8-symbol window; only SF5/SF6, which this firmware
  // never runs, take 12.
  TEST_ASSERT_EQUAL_UINT16(8,  Airtime::rxDutyCycleMinSymbols(7));
  TEST_ASSERT_EQUAL_UINT16(8,  Airtime::rxDutyCycleMinSymbols(12));
  TEST_ASSERT_EQUAL_UINT16(12, Airtime::rxDutyCycleMinSymbols(6));
  // With a TCXO the driver will not sleep for under 6016 us. Written out so
  // the threshold every case below is measured against is visible once.
  TEST_ASSERT_EQUAL_UINT32(6016,
                           Airtime::RX_DC_TCXO_DELAY_US + Airtime::RX_DC_TRANSITION_US);
}

// The shipped default channel. This is the case that matters most, because it
// is the one every node is in until someone changes it: the sleep is 4096 us
// against a 6016 us threshold, so the feature does nothing at all.
void test_the_default_channel_does_not_duty_cycle_at_all() {
  const uint32_t sleepUs = Airtime::rxDutyCycleSleepUs(8, 125.0f, 18);
  TEST_ASSERT_EQUAL_UINT32(4096, sleepUs);            // 2 symbols of 2048 us
  TEST_ASSERT_FALSE_MESSAGE(Airtime::rxDutyCycleEngages(sleepUs),
                            "SF8/125 kHz must fall back to a continuous receive");
}

// One spreading factor up doubles the symbol and clears the threshold.
void test_one_spreading_factor_higher_engages() {
  const uint32_t sleepUs = Airtime::rxDutyCycleSleepUs(9, 125.0f, 18);
  TEST_ASSERT_EQUAL_UINT32(8192, sleepUs);            // 2 symbols of 4096 us
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(sleepUs));
  // ...and everything slower than it does too.
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(12, 125.0f, 18)));
}

// It is the symbol time that decides, not the spreading factor: a narrow
// channel gets there at the lowest factor this firmware allows.
void test_a_narrow_channel_engages_at_a_low_spreading_factor() {
  const uint32_t sleepUs = Airtime::rxDutyCycleSleepUs(7, 31.25f, 18);
  TEST_ASSERT_EQUAL_UINT32(8192, sleepUs);            // 2 symbols of 4096 us
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(sleepUs));
  // The crossover sits at a 3008 us symbol, i.e. a 6016 us sleep over two
  // symbols. SF8 at 62.5 kHz is over it; SF8 at 125 kHz is under.
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(8, 62.5f, 18)));
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(8, 125.0f, 18)));
}

// A preamble no longer than the two sampling windows leaves nothing to sleep
// through, and the subtraction must floor rather than wrap: 8 - 16 in unsigned
// arithmetic is a sleep of several minutes.
void test_a_short_preamble_leaves_no_room_to_sleep() {
  TEST_ASSERT_EQUAL_UINT32(0, Airtime::rxDutyCycleSleepUs(12, 125.0f, 16));  // exactly 2*8
  TEST_ASSERT_EQUAL_UINT32(0, Airtime::rxDutyCycleSleepUs(12, 125.0f, 8));   // under it
  TEST_ASSERT_EQUAL_UINT32(0, Airtime::rxDutyCycleSleepUs(12, 125.0f, 0));
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(0));
  // One symbol over the floor is a real, if short, sleep.
  TEST_ASSERT_EQUAL_UINT32(32768, Airtime::rxDutyCycleSleepUs(12, 125.0f, 17));
}

// A board with no TCXO leaves the driver's delay at zero, which drops the
// threshold to the 1016 us transition alone — the default channel would then
// engage. The caller passes the delay in for exactly this reason.
void test_the_tcxo_ramp_is_part_of_the_threshold() {
  const uint32_t sleepUs = Airtime::rxDutyCycleSleepUs(8, 125.0f, 18);   // 4096
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(sleepUs));               // 6016 us threshold
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(sleepUs, 0));             // 1016 us threshold
  // Exactly at the threshold counts as engaging, as the driver's own
  // comparison does (it falls back only when strictly shorter).
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(6016));
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(6015));
}

// An explicit minSymbols overrides the per-SF default, and a bandwidth that
// could not have come from a validated setting answers rather than dividing.
void test_min_symbols_and_a_nonsense_bandwidth() {
  // 18 - 2*4 = 10 symbols of 2048 us at the default channel.
  TEST_ASSERT_EQUAL_UINT32(20480, Airtime::rxDutyCycleSleepUs(8, 125.0f, 18, 4));
  TEST_ASSERT_EQUAL_UINT32(0, Airtime::rxDutyCycleSleepUs(8, 0.0f, 18));
  TEST_ASSERT_EQUAL_UINT32(0, Airtime::rxDutyCycleSleepUs(8, -125.0f, 18));
}

// The symbol time is a truncating integer divide in the driver, not a rounded
// one, and every case above happens to divide exactly — so none of them can
// tell the two apart. 41.7 kHz does not: 1 280 000 / 417 is 3069.54, which the
// driver truncates to 3069, and a rounding implementation would report a 6140 us
// sleep where the chip is actually given 6138.
void test_the_symbol_time_truncates_rather_than_rounds() {
  TEST_ASSERT_EQUAL_UINT32(6138, Airtime::rxDutyCycleSleepUs(7, 41.7f, 18));
  // Comfortably over the 6016 us threshold either way, so the difference is
  // only ever visible as the reported figure — which is what STATUS prints.
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(7, 41.7f, 18)));
}

// The threshold has a ceiling as well as a floor, and the two failures are not
// alike. Under the floor RadioLib arms a plain continuous receive and the node
// hears normally. Over the ceiling the sleep period no longer fits the 24 bits
// SetRxDutyCycle carries, so startReceiveDutyCycle() returns
// RADIOLIB_ERR_INVALID_SLEEP_PERIOD *before* it stages a mode — arming nothing
// at all and leaving the chip in standby, deaf. Only mirroring the first gate
// would have told a caller to go ahead and do exactly that.
//
// It is reachable from settings this firmware accepts: 7.8 kHz is in the
// SX1262's bandwidth list, SF12 is inside its range, and the validator takes a
// preamble up to 1000. At SF12/7.8 kHz a symbol is 525 128 us, so the boundary
// falls between a 515- and a 516-symbol preamble.
void test_a_sleep_too_long_for_the_chip_is_not_engagement() {
  const uint32_t justUnder = Airtime::rxDutyCycleSleepUs(12, 7.8f, 515);
  const uint32_t justOver  = Airtime::rxDutyCycleSleepUs(12, 7.8f, 516);
  TEST_ASSERT_EQUAL_UINT32(262038872, justUnder);      // 499 symbols
  TEST_ASSERT_EQUAL_UINT32(262564000, justOver);       // 500 symbols
  TEST_ASSERT_TRUE_MESSAGE(Airtime::rxDutyCycleEngages(justUnder),
                           "the last preamble that still fits 24 bits must engage");
  TEST_ASSERT_FALSE_MESSAGE(Airtime::rxDutyCycleEngages(justOver),
                            "a sleep the driver would refuse must never read as engaged");
  // A wider channel moves the boundary but not the rule: at 10.4 kHz the
  // symbol is shorter, so it takes more preamble to overflow — 682, not 516.
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(12, 10.4f, 681)));
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(Airtime::rxDutyCycleSleepUs(12, 10.4f, 682)));
  // The ceiling is the chip's, not the TCXO's: dropping the ramp to zero shifts
  // the floor by 5 ms and leaves the boundary where it was.
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(justUnder, 0));
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(justOver, 0));
}

// The bound on one CAD probe. Not a prediction of how long a scan takes — a
// deadline past which the chip is not answering — so what is pinned here is the
// derivation, at both ends of the channel range the firmware accepts.
//
// Sixteen symbol times plus a fixed 20 ms: sixteen because the longest scan any
// of the four drivers arms is the SX128x's 8 symbols and this leaves the same
// margin again over it, and the 20 ms because the scan is bracketed by standby
// transitions, a TCXO ramp of about 5 ms on the boards that have one, the SPI
// traffic that carries the commands and a scheduling tick either side.

// What the four drivers actually arm, in symbols (RadioLib 7.7.1) — the figures
// Airtime.h's comment is written against, named here so the margin can be
// asserted against them rather than against a number repeated from the header.
static const uint16_t kDriverScanSymbolsSX126x = 4;
static const uint16_t kDriverScanSymbolsSX128x = 8;
static const uint16_t kDriverScanSymbolsLR11x0 = 2;
static const uint16_t kDriverScanSymbolsSX127x = 1;   // hardware-timed, about one symbol
static const uint16_t kLongestDriverScanSymbols = kDriverScanSymbolsSX128x;
static const uint16_t kCadSafetyFactor = 2;           // "twice the longest driver scan"

void test_the_cad_deadline_follows_the_channel() {
  // The shipped default: 2.048 ms symbols, so 32.8 + 20.
  TEST_ASSERT_EQUAL_UINT32(53, make(8, 125.0f).cadTimeoutMs());
  // ...and it moves with the channel the way the slot does. SF7 halves the
  // symbol, SF12 is thirty-two times the SF7 figure.
  TEST_ASSERT_EQUAL_UINT32(36,  make(7, 125.0f).cadTimeoutMs());
  TEST_ASSERT_EQUAL_UINT32(544, make(12, 125.0f).cadTimeoutMs());
  // Bandwidth is the other half of the symbol time, and it pulls the same way.
  TEST_ASSERT_EQUAL_UINT32(1069, make(12, 62.5f).cadTimeoutMs());

  // Comfortably clear of the scan it is bounding — and that has to be asserted
  // on the symbol-derived part alone. Comparing the whole deadline against the
  // longest driver scan carries almost no weight, because the fixed 20 ms is
  // in it: at SF7/125 kHz the old form was satisfied all the way down to 7.39
  // symbols of scan, so it passed unchanged with CAD_SYMBOLS cut from 16 to 8
  // and only the hard-pinned figures above would have caught the regression.
  //
  // First the constant itself, against the drivers' own counts rather than a
  // number copied out of the header.
  TEST_ASSERT_TRUE(kLongestDriverScanSymbols >= kDriverScanSymbolsSX126x);
  TEST_ASSERT_TRUE(kLongestDriverScanSymbols >= kDriverScanSymbolsLR11x0);
  TEST_ASSERT_TRUE(kLongestDriverScanSymbols >= kDriverScanSymbolsSX127x);
  TEST_ASSERT_TRUE_MESSAGE(
      Airtime::CAD_SYMBOLS >= kCadSafetyFactor * kLongestDriverScanSymbols,
      "CAD_SYMBOLS must be twice the longest scan any of the four drivers arms");

  // ...then the derivation, on every unclamped channel: take the fixed
  // allowance back off and what is left must still cover twice the longest
  // driver scan on that channel. The right-hand side truncates where
  // cadTimeoutMs() rounds, which can only favour the left, so the comparison is
  // exact rather than approximate.
  for (uint8_t sf = 7; sf <= 12; sf++) {
    Airtime a = make(sf, 125.0f);
    const uint32_t scanMs = (uint32_t)((float)(kCadSafetyFactor * kLongestDriverScanSymbols)
                                       * a.symbolTimeMs());
    TEST_ASSERT_TRUE_MESSAGE(a.cadTimeoutMs() >= Airtime::CAD_OVERHEAD_MS + scanMs,
                             "the deadline must clear twice the longest driver scan");
  }
}

// Both clamps, and why each is where it is.
void test_the_cad_deadline_is_clamped_at_both_ends() {
  // Fast channels: the symbols all but vanish and the fixed allowance is the
  // whole figure, which would otherwise leave a deadline shorter than one
  // scheduling round-trip.
  TEST_ASSERT_EQUAL_UINT32(Airtime::CAD_TIMEOUT_MIN_MS, make(5, 500.0f).cadTimeoutMs());
  TEST_ASSERT_EQUAL_UINT32(Airtime::CAD_TIMEOUT_MIN_MS, make(7, 500.0f).cadTimeoutMs());

  // The slowest channel the settings will accept: SF12 at 7.8 kHz, where one
  // symbol is 525 ms and sixteen of them are 8.4 s — longer than the whole CSMA
  // deferral the probe is part of. Clamped, it is still four times the 2.1 s
  // scan the driver actually arms there.
  Airtime slow = make(12, 7.8f);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 525.1f, slow.symbolTimeMs());
  TEST_ASSERT_EQUAL_UINT32(Airtime::CAD_TIMEOUT_MAX_MS, slow.cadTimeoutMs());
  TEST_ASSERT_TRUE_MESSAGE((float)Airtime::CAD_TIMEOUT_MAX_MS > 4.0f * slow.symbolTimeMs(),
                           "the ceiling must still clear the longest real scan");

  // Monotonic in the symbol time up to the clamp, so no channel between the two
  // ends gets a deadline shorter than a faster one's.
  uint32_t previous = 0;
  for (uint8_t sf = 7; sf <= 12; sf++) {
    const uint32_t t = make(sf, 125.0f).cadTimeoutMs();
    TEST_ASSERT_TRUE(t >= previous);
    previous = t;
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_time_on_air_matches_datasheet);
  RUN_TEST(test_symbol_time_and_slot);
  RUN_TEST(test_the_cad_deadline_follows_the_channel);
  RUN_TEST(test_the_cad_deadline_is_clamped_at_both_ends);
  RUN_TEST(test_long_term_util_over_the_hour);
  RUN_TEST(test_duty_cycle_lock_and_release);
  RUN_TEST(test_budget_used_reports_fraction_of_allowance);
  RUN_TEST(test_band_lookup_covers_the_eu_plan);
  RUN_TEST(test_channel_straddling_a_boundary_takes_the_stricter_band);
  RUN_TEST(test_effective_limit_follows_the_band);
  RUN_TEST(test_bins_expire_rather_than_accumulate);
  RUN_TEST(test_contention_window_widens_with_channel_use);
  RUN_TEST(test_short_term_util_uses_recent_bins_only);
  RUN_TEST(test_the_driver_constants_are_the_ones_we_mirror);
  RUN_TEST(test_the_default_channel_does_not_duty_cycle_at_all);
  RUN_TEST(test_one_spreading_factor_higher_engages);
  RUN_TEST(test_a_narrow_channel_engages_at_a_low_spreading_factor);
  RUN_TEST(test_a_short_preamble_leaves_no_room_to_sleep);
  RUN_TEST(test_the_tcxo_ramp_is_part_of_the_threshold);
  RUN_TEST(test_min_symbols_and_a_nonsense_bandwidth);
  RUN_TEST(test_the_symbol_time_truncates_rather_than_rounds);
  RUN_TEST(test_a_sleep_too_long_for_the_chip_is_not_engagement);
  return UNITY_END();
}
