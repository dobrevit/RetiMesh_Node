// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// Airtime's duty-cycled-receive arithmetic, against RadioLib's own, on the
// host: pio test -e native
//
// Airtime::rxDutyCycleSleepUs() and rxDutyCycleEngages() are a hand copy of
// three pieces of driver: PhysicalLayer::calculateRxDutyCycle, the
// `tcxoDelay + 1016` threshold in SX126x::startReceiveDutyCycleAuto, and the
// range check in SX126x::startReceiveDutyCycle. test_airtime pins them against
// RetiMesh's own named constants, which catches a typo here but cannot catch
// the failure that matters: a RadioLib bump that moves the threshold. Then the
// node would go on reporting `armed=true` on a channel where the driver had
// silently substituted a plain startReceive() — precisely the lie the whole
// rx_duty_cycle feature exists to prevent — with every test still green.
//
// So this suite asks the library instead of restating it. Two of the three
// pieces can be reached without a bus:
//
//   * calculateRxDutyCycle() is public on PhysicalLayer and is pure
//     arithmetic — no SPI, no chip state — so the sleep period can be compared
//     against the driver's own answer across the whole settings space;
//   * startReceiveDutyCycle() does its two range checks before it stages any
//     mode, so calling it in the *rejecting* direction returns the driver's own
//     refusal without ever reaching the bus. Only that direction: an accepted
//     period runs on into stageMode(), and the Module below has a null HAL.
//
// What is left over is named at the bottom of the file, because a partial pin
// that reads as a complete one is worse than none.
#include <unity.h>
#include <RadioLib.h>
#include "Config.h"             // RF_PREAMBLE_SYMS — the network's preamble floor
#include "RadioRxArmPolicy.h"   // RadioRxArm::sizingPreamble — which preamble we size on
#include "Airtime.cpp"

// Never touched: the constructors only store the pointer, and nothing here
// reaches the bus. begin() is what would need a HAL, and is never called —
// which also means setTCXO() is never called, so this part's private tcxoDelay
// is the 0 its declaration gives it (SX126x.h) and the driver's transition
// arithmetic below runs at zero TCXO ramp.
static Module mod(nullptr, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
static SX1262 sx1262(&mod);

// The driver's answer for one channel, with the sender's preamble and this
// node's configured preamble given separately — which is the distinction the
// whole arm turns on. minSymbols 0 selects the driver's own per-SF default.
static uint32_t driverSleepUs2(uint8_t sf, float bwKhz, uint16_t senderSyms,
                               uint16_t configuredSyms, uint16_t minSymbols,
                               int16_t* stateOut = nullptr) {
  DataRate_t dr = {};
  dr.lora.spreadingFactor = sf;
  dr.lora.bandwidth       = bwKhz;
  dr.lora.codingRate      = 5;
  uint32_t wake = 0, sleep = 0;
  const int16_t state = sx1262.calculateRxDutyCycle(senderSyms, configuredSyms,
                                                    minSymbols, &dr, &wake, &sleep);
  if (stateOut) *stateOut = state;
  return state == RADIOLIB_ERR_NONE ? sleep : 0;
}

// The same where the two preambles are equal, which is the shape the pure
// arithmetic pins below are about.
static uint32_t driverSleepUs(uint8_t sf, float bwKhz, uint16_t preambleSyms,
                              uint16_t minSymbols, int16_t* stateOut = nullptr) {
  return driverSleepUs2(sf, bwKhz, preambleSyms, preambleSyms, minSymbols, stateOut);
}

// One symbol on this channel, by the driver's own truncating expression. Used
// to say "the sleep is longer than a whole conforming preamble", which is what
// a missed packet actually looks like.
static uint32_t symbolUs(uint8_t sf, float bwKhz) {
  return (uint32_t)((float)((uint32_t)10000 << sf) / (10.0f * bwKhz));
}

// Every bandwidth the SX1262 offers and every spreading factor this firmware
// will configure, so a driver that changed its symbol-time expression for one
// corner of the space still fails here.
static const float    kBw[]  = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f,
                                 62.5f, 125.0f, 250.0f, 500.0f };
static const uint16_t kPre[] = { 0, 1, 8, 15, 16, 17, 18, 24, 32, 64, 128,
                                 515, 516, 681, 682, 1000 };

// The core pin: our sleep is the driver's sleep, everywhere. This is one
// assertion over ~1300 channels rather than a handful of worked examples,
// because the expression has four inputs and the ways to get it subtly wrong
// (a truncation, an off-by-one in the two wake windows, the wrong minSymbols
// for a spreading factor) each show up in only part of the space.
static void test_our_sleep_period_is_the_drivers_own() {
  for (uint8_t sf = 5; sf <= 12; sf++) {
    for (size_t b = 0; b < sizeof(kBw) / sizeof(kBw[0]); b++) {
      for (size_t p = 0; p < sizeof(kPre) / sizeof(kPre[0]); p++) {
        int16_t state = RADIOLIB_ERR_UNKNOWN;
        const uint32_t theirs = driverSleepUs(sf, kBw[b], kPre[p], 0, &state);
        TEST_ASSERT_EQUAL_INT16_MESSAGE(RADIOLIB_ERR_NONE, state,
                                        "the sender preamble is the configured one, so this "
                                        "call must never be refused");
        const uint32_t ours = Airtime::rxDutyCycleSleepUs(sf, kBw[b], kPre[p]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(theirs, ours,
                                         "Airtime::rxDutyCycleSleepUs no longer matches "
                                         "PhysicalLayer::calculateRxDutyCycle");
      }
    }
  }
}

// The same, with the override the API exposes, because rxDutyCycleSleepUs()
// takes minSymbols and armReceive() promises the driver's default is what was
// modelled. A minSymbols the driver reads differently would move the boundary
// without moving any constant named in this repository.
static void test_our_sleep_period_is_the_drivers_own_with_an_override() {
  static const uint16_t kMin[] = { 1, 4, 8, 9, 12, 16, 64 };
  for (uint8_t sf = 5; sf <= 12; sf++) {
    for (size_t m = 0; m < sizeof(kMin) / sizeof(kMin[0]); m++) {
      for (size_t p = 0; p < sizeof(kPre) / sizeof(kPre[0]); p++) {
        const uint32_t theirs = driverSleepUs(sf, 125.0f, kPre[p], kMin[m]);
        const uint32_t ours   = Airtime::rxDutyCycleSleepUs(sf, 125.0f, kPre[p], kMin[m]);
        TEST_ASSERT_EQUAL_UINT32(theirs, ours);
      }
    }
  }
}

// RX_DC_MIN_SYMBOLS_SF7 / _SF6, pinned against the driver rather than against
// themselves. Asking the driver for its default and then for our mirror of its
// default must produce the same sleep on a preamble long enough for the two to
// differ; the SF5/SF6 split is where a driver that retuned this would show.
static void test_the_minimum_symbol_counts_are_the_drivers_own() {
  for (uint8_t sf = 5; sf <= 12; sf++) {
    const uint16_t mirrored = Airtime::rxDutyCycleMinSymbols(sf);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(driverSleepUs(sf, 125.0f, 64, 0),
                                     driverSleepUs(sf, 125.0f, 64, mirrored),
                                     "Airtime::rxDutyCycleMinSymbols is not the driver's "
                                     "own default for this spreading factor");
    // ...and that it is a real choice, not a value that happens to agree
    // because the preamble swallows the difference: one symbol either way
    // changes the answer.
    TEST_ASSERT_NOT_EQUAL_UINT32(driverSleepUs(sf, 125.0f, 64, 0),
                                 driverSleepUs(sf, 125.0f, 64, (uint16_t)(mirrored + 1)));
  }
  // The split itself: 12 below SF7, 8 from SF7 up. Read off the driver, by
  // finding which mirrored count reproduces its default.
  TEST_ASSERT_EQUAL_UINT16(Airtime::RX_DC_MIN_SYMBOLS_SF6, Airtime::rxDutyCycleMinSymbols(6));
  TEST_ASSERT_EQUAL_UINT16(Airtime::RX_DC_MIN_SYMBOLS_SF7, Airtime::rxDutyCycleMinSymbols(7));
}

// The contract comment on Airtime::rxDutyCycleEngages() says the predicate is
// valid for one call shape only, because a sender preamble longer than the
// configured one is refused before any of this arithmetic runs. Pinned so that
// the claim keeps meaning what it says.
static void test_a_longer_sender_preamble_is_refused_before_the_arithmetic() {
  DataRate_t dr = {};
  dr.lora.spreadingFactor = 9;
  dr.lora.bandwidth       = 125.0f;
  dr.lora.codingRate      = 5;
  uint32_t wake = 0, sleep = 0;
  TEST_ASSERT_EQUAL_INT16(RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH,
                          sx1262.calculateRxDutyCycle(19, 18, 0, &dr, &wake, &sleep));
  TEST_ASSERT_EQUAL_INT16(RADIOLIB_ERR_NONE,
                          sx1262.calculateRxDutyCycle(18, 18, 0, &dr, &wake, &sleep));
}

// ---------------------------------------------------------------------------
//  Which preamble the window is sized on, put to the driver rather than argued.
//  RadioRxArm::sizingPreamble() is the rule and test_radio_rx_arm pins the rule
//  itself; what is pinned here is that it produces a call this driver accepts,
//  and a sleep short enough to still catch a conforming sender.
// ---------------------------------------------------------------------------

// The regression, stated the way the driver states it. A node configured to 64
// symbols that sized its window on 64 would sleep for 48 symbol times, while a
// conforming RNode-lineage peer transmits RF_PREAMBLE_SYMS of preamble — 18
// symbol times fit inside 48 with room to spare, so the peer's whole preamble
// can land in the sleep and the packet is never heard. Sized on the floor the
// window is two symbols and cannot swallow it.
static void test_sizing_on_the_local_preamble_would_sleep_past_a_conforming_sender() {
  const uint8_t sf = 10; const float bw = 125.0f;         // a channel that engages
  const uint32_t peerPreambleUs = (uint32_t)RF_PREAMBLE_SYMS * symbolUs(sf, bw);

  const uint32_t naive = driverSleepUs(sf, bw, 64, 0);    // the defect: our own setting
  TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(peerPreambleUs, naive,
      "sizing on a 64-symbol local preamble sleeps longer than a conforming sender's "
      "entire preamble - that is the missed packet");

  const uint32_t sized =
      driverSleepUs2(sf, bw, RadioRxArm::sizingPreamble(64, RF_PREAMBLE_SYMS), 64, 0);
  TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(peerPreambleUs, sized,
      "sized on the floor, the receiver is awake inside every conforming preamble");
  // ...and it is exactly the window a node left at the floor would use, which
  // is the point: the local setting no longer moves it.
  TEST_ASSERT_EQUAL_UINT32(driverSleepUs(sf, bw, RF_PREAMBLE_SYMS, 0), sized);
}

// The published figure and the armed figure are one number, across the whole
// space the validator accepts. configureAirtime() derives it once and hands it
// both to Airtime::rxDutyCycleSleepUs() and to startReceiveDutyCycleAuto();
// this asks the driver what that second call computes and requires the first to
// agree, so rx_duty_cycle_sleep_us can never describe a sleep the chip was not
// programmed with.
static void test_what_the_node_publishes_is_what_the_arm_call_computes() {
  static const uint16_t kCfg[] = { 6, 8, 12, 17, 18, 19, 24, 64, 128, 516, 1000 };
  for (uint8_t sf = 5; sf <= 12; sf++) {
    for (size_t b = 0; b < sizeof(kBw) / sizeof(kBw[0]); b++) {
      for (size_t c = 0; c < sizeof(kCfg) / sizeof(kCfg[0]); c++) {
        const uint16_t sized = RadioRxArm::sizingPreamble(kCfg[c], RF_PREAMBLE_SYMS);
        int16_t state = RADIOLIB_ERR_UNKNOWN;
        const uint32_t theirs = driverSleepUs2(sf, kBw[b], sized, kCfg[c], 0, &state);
        TEST_ASSERT_EQUAL_INT16_MESSAGE(RADIOLIB_ERR_NONE, state,
            "the sizing preamble must never exceed the configured one, or the arm is "
            "refused and the receiver is left unarmed");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(theirs,
            Airtime::rxDutyCycleSleepUs(sf, kBw[b], sized),
            "the published sleep is not the sleep the arm call computes");
      }
    }
  }
}

// A node configured below the floor sizes on its own shorter preamble — it has
// to, because the driver refuses a sender preamble past the configured one —
// and at the bottom of the accepted range there is nothing left to sleep
// through: two wake windows of eight symbols each already cover six, so the
// sleep is zero and the mode cannot engage on any channel. Right rather than
// unfortunate: such a node is outside the interop guarantee to begin with.
static void test_a_preamble_below_the_floor_shortens_the_window_and_stops_engaging() {
  for (uint16_t cfg = 6; cfg < (uint16_t)RF_PREAMBLE_SYMS; cfg++) {
    const uint16_t sized = RadioRxArm::sizingPreamble(cfg, RF_PREAMBLE_SYMS);
    TEST_ASSERT_EQUAL_UINT16(cfg, sized);
    for (uint8_t sf = 5; sf <= 12; sf++) {
      for (size_t b = 0; b < sizeof(kBw) / sizeof(kBw[0]); b++) {
        const uint32_t ours = Airtime::rxDutyCycleSleepUs(sf, kBw[b], sized);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(driverSleepUs2(sf, kBw[b], sized, cfg, 0), ours,
                                         "below the floor we still mirror the driver");
        // Sixteen symbols or fewer leave nothing between the two wake windows
        // at any spreading factor, so there is no saving to claim.
        if (cfg <= 16)
          TEST_ASSERT_FALSE_MESSAGE(Airtime::rxDutyCycleEngages(ours),
              "a preamble no longer than the two wake windows has no sleep in it");
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  The range check, asked of the driver in the only direction that is safe to
//  ask. Both boundaries below are computed from RetiMesh's constants and then
//  put to RadioLib: if the driver's own ceiling or compensation moved, the
//  value we call "the driver will refuse this" would no longer be refused.
// ---------------------------------------------------------------------------

// The smallest sleep our mirror says will not fit the chip's 24-bit tick
// counter, at zero TCXO ramp. Derived, not written down, so the two constants
// are what this test is about.
static uint32_t firstSleepOverTheCeiling() {
  return Airtime::RX_DC_COMPENSATION_US +
         (((uint32_t)Airtime::RX_DC_PERIOD_RAW_MAX + 1UL) * 125UL) / 8UL;
}

static void test_the_driver_refuses_the_sleep_our_ceiling_predicts() {
  const uint32_t over = firstSleepOverTheCeiling();          // 262 145 000 us
  // Our side first: over is refused, one microsecond under it is taken.
  TEST_ASSERT_FALSE(Airtime::rxDutyCycleEngages(over, 0));
  TEST_ASSERT_TRUE(Airtime::rxDutyCycleEngages(over - 1, 0));

  // ...and the driver's, with a wake period well inside its own check so that
  // the sleep check is what answers. Only this direction is asked: `over - 1`
  // would be accepted and would run on into stageMode() and the null HAL.
  TEST_ASSERT_EQUAL_INT16_MESSAGE(RADIOLIB_ERR_INVALID_SLEEP_PERIOD,
                                  sx1262.startReceiveDutyCycle(100000, over),
                                  "RX_DC_PERIOD_RAW_MAX or RX_DC_COMPENSATION_US no longer "
                                  "matches SX126x::startReceiveDutyCycle");
  // Comfortably past it too, which is the underflow-free half of the same check.
  TEST_ASSERT_EQUAL_INT16(RADIOLIB_ERR_INVALID_SLEEP_PERIOD,
                          sx1262.startReceiveDutyCycle(100000, over * 2));
}

// The other end of the same check, and the one that props up the 1016 we
// cannot read: at zero TCXO ramp every sleep shorter than RX_DC_TRANSITION_US
// divides to a raw period of zero (or underflows the compensation), and the
// driver refuses it outright. That is why the threshold it compares against is
// `tcxoDelay + 1016` and not some other number — 1016 us is the first sleep the
// range check behind it can accept.
static void test_the_driver_refuses_every_sleep_below_the_transition() {
  static const uint32_t kUnder[] = { 0, 1, 999, 1000, 1001, 1008,
                                     Airtime::RX_DC_TRANSITION_US - 1 };
  for (size_t i = 0; i < sizeof(kUnder) / sizeof(kUnder[0]); i++) {
    TEST_ASSERT_FALSE_MESSAGE(Airtime::rxDutyCycleEngages(kUnder[i], 0),
                              "our predicate must not promise a sleep this short");
    TEST_ASSERT_EQUAL_INT16_MESSAGE(RADIOLIB_ERR_INVALID_SLEEP_PERIOD,
                                    sx1262.startReceiveDutyCycle(100000, kUnder[i]),
                                    "the driver's range check no longer bottoms out at "
                                    "RX_DC_TRANSITION_US");
  }
}

// ---------------------------------------------------------------------------
//  What is still a hand mirror, stated rather than implied.
//
//   * RX_DC_TRANSITION_US (1016) is a literal inside the body of
//     SX126x::startReceiveDutyCycleAuto — `sleepPeriod < this->tcxoDelay + 1016`
//     — with no symbol on it. It cannot be read, and the call cannot be made:
//     startReceiveDutyCycleAuto reaches the bus on both sides of that branch
//     (startReceive() below it, startReceiveDutyCycle() above). The test above
//     pins its floor — the range check it guards refuses everything under
//     1016 — but a bump that moved only the literal would not fail here.
//   * RX_DC_TCXO_DELAY_US (5000) is a default argument,
//     `setTCXO(float voltage, uint32_t delay = 5000)`. Default arguments are
//     not introspectable in C++, and setTCXO() itself writes to the chip, so
//     this stays a plain mirror of the header.
//
//  Both are restated below so that a RadioLib bump at least has to be looked
//  at: the assertion is against the header this suite was written from.
// ---------------------------------------------------------------------------
static void test_the_two_values_that_cannot_be_reached_are_restated() {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1016, Airtime::RX_DC_TRANSITION_US,
                                   "hand mirror of SX126x.cpp's `tcxoDelay + 1016`");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(5000, Airtime::RX_DC_TCXO_DELAY_US,
                                   "hand mirror of SX126x.h's setTCXO() default delay");
  // The pair the driver deliberately writes as two different literals. Folding
  // them would misreport the boundary in whichever direction the fold went.
  TEST_ASSERT_NOT_EQUAL_UINT32(Airtime::RX_DC_TRANSITION_US, Airtime::RX_DC_COMPENSATION_US);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_our_sleep_period_is_the_drivers_own);
  RUN_TEST(test_our_sleep_period_is_the_drivers_own_with_an_override);
  RUN_TEST(test_the_minimum_symbol_counts_are_the_drivers_own);
  RUN_TEST(test_a_longer_sender_preamble_is_refused_before_the_arithmetic);
  RUN_TEST(test_sizing_on_the_local_preamble_would_sleep_past_a_conforming_sender);
  RUN_TEST(test_what_the_node_publishes_is_what_the_arm_call_computes);
  RUN_TEST(test_a_preamble_below_the_floor_shortens_the_window_and_stops_engaging);
  RUN_TEST(test_the_driver_refuses_the_sleep_our_ceiling_predicts);
  RUN_TEST(test_the_driver_refuses_every_sleep_below_the_transition);
  RUN_TEST(test_the_two_values_that_cannot_be_reached_are_restated);
  return UNITY_END();
}
