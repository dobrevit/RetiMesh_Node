// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
//
// Which receive the radio arms, and what it is then allowed to claim.
// Runs on the host: pio test -e native
//
// The rule has three inputs that nothing on a bench can vary quickly — the
// operator's switch, the fitted transceiver, and whether the channel yields a
// sleep RadioLib will take — and two of its four answers look identical from
// the outside: a node in continuous receive because nobody asked and a node in
// continuous receive because the chip cannot behave differently on the air.
// Pinning the table here is the only place the difference is cheap to check.
//
// Two failures it exists to catch, both of which have shipped elsewhere in this
// firmware in other forms:
//
//   * a channel the mode does not suit being handed to the driver anyway. The
//     driver accepts it, and behind that one acceptance sit a harmless
//     fallback to continuous receive and an outright refusal that arms nothing
//     at all and leaves the node deaf. Airtime's predicate cannot tell them
//     apart, so the rule must not ask (RadioRxArmPolicy.h).
//   * armed being derived from the plan rather than from what the driver
//     accepted. That is a node reporting a saving it is not making, which is
//     the whole reason the field exists.
//   * the sleep window sized on this node's own preamble instead of the one the
//     network guarantees, which sleeps straight through a conforming peer's
//     transmission while every surface reports the mode working.
#include <unity.h>
#include "Config.h"            // RF_PREAMBLE_SYMS — the floor the rule sizes on
#include "RadioRxArmPolicy.h"

// --- the four outcomes, one per reason --------------------------------------

// The shipped default, on every board. Nothing else is consulted: an SX1262 on
// a channel that suits the mode perfectly still listens continuously until
// somebody turns the switch on.
static void test_the_switch_off_means_continuous_whatever_else_is_true() {
  for (int capable = 0; capable <= 1; capable++) {
    for (int engages = 0; engages <= 1; engages++) {
      const RadioRxArm::Plan p = RadioRxArm::decide(false, capable != 0, engages != 0);
      TEST_ASSERT_EQUAL_MESSAGE((int)RadioRxArm::Plan::Continuous, (int)p,
                                "off must mean off, on every chip and every channel");
      TEST_ASSERT_FALSE(RadioRxArm::dutyCycled(p));
    }
  }
}

// The setting is accepted on every board so that one export provisions a mixed
// fleet, which means an SX1276, an SX1280 or an LR1110 can be running with it
// on. They arm the same continuous receive — and say a different thing about
// why, because "this radio has no such mode" is not a channel the operator can
// fix by retuning.
static void test_a_chip_without_the_mode_stays_continuous_and_says_so() {
  for (int engages = 0; engages <= 1; engages++) {
    const RadioRxArm::Plan p = RadioRxArm::decide(true, false, engages != 0);
    TEST_ASSERT_EQUAL_MESSAGE((int)RadioRxArm::Plan::Unsupported, (int)p,
                              "the chip is the reason, and it is not the channel's fault");
    TEST_ASSERT_FALSE(RadioRxArm::dutyCycled(p));
  }
}

// The case the whole policy file exists for, and the shipped SF8/125 kHz
// channel's case: asked for, supported, and the sleep is one RadioLib will not
// take. We arm the continuous receive ourselves rather than letting the driver
// decide, because one of the two ways it can decline leaves the chip in standby
// and the node deaf.
static void test_a_channel_the_driver_will_not_sleep_on_is_not_offered_to_it() {
  const RadioRxArm::Plan p = RadioRxArm::decide(true, true, false);
  TEST_ASSERT_EQUAL_MESSAGE((int)RadioRxArm::Plan::ChannelUnsuitable, (int)p,
                            "a sleep the driver refuses must never reach the driver");
  TEST_ASSERT_FALSE_MESSAGE(RadioRxArm::dutyCycled(p),
                            "asking anyway risks an unarmed receiver for no saving");
}

// All three agree: an SX1262, the switch on, and a channel whose sleep clears
// the wake-up transition without overflowing the chip's 24-bit period.
static void test_all_three_agreeing_is_the_only_way_to_the_duty_cycled_arm() {
  const RadioRxArm::Plan p = RadioRxArm::decide(true, true, true);
  TEST_ASSERT_EQUAL((int)RadioRxArm::Plan::DutyCycled, (int)p);
  TEST_ASSERT_TRUE(RadioRxArm::dutyCycled(p));

  // ...and it is the only way. Anything with a false in it arms continuous.
  for (int a = 0; a <= 1; a++)
    for (int b = 0; b <= 1; b++)
      for (int c = 0; c <= 1; c++)
        TEST_ASSERT_EQUAL_MESSAGE(a && b && c,
                                  RadioRxArm::dutyCycled(RadioRxArm::decide(a != 0, b != 0, c != 0)),
                                  "the duty-cycled arm needs the switch, the chip and the channel");
}

// --- what may then be published --------------------------------------------

// armed is what the driver accepted, never what the rule predicted. A driver
// that refuses the call — a library bump moving a threshold, a chip that has
// stopped answering — leaves the node in a continuous receive, and the field an
// operator reads to tell a working feature from a silently inactive one has to
// say so.
static void test_a_refused_arm_is_not_an_armed_receiver() {
  const RadioRxArm::Plan p = RadioRxArm::decide(true, true, true);
  TEST_ASSERT_TRUE_MESSAGE(RadioRxArm::armed(p, true),
                           "the driver took it: the saving is being made");
  TEST_ASSERT_FALSE_MESSAGE(RadioRxArm::armed(p, false),
                            "a refused call is a continuous receive, whatever the plan said");
}

// And in the other direction: the three continuous plans never make a call, so
// no answer from the driver can make them claim the mode. This is the guard
// against wiring armed to the wrong thing — the setting, or the prediction —
// which would report the saving on every node that merely asked for it.
static void test_the_continuous_plans_can_never_report_armed() {
  const RadioRxArm::Plan continuous[] = {
    RadioRxArm::Plan::Continuous,
    RadioRxArm::Plan::Unsupported,
    RadioRxArm::Plan::ChannelUnsuitable,
  };
  for (RadioRxArm::Plan p : continuous) {
    TEST_ASSERT_FALSE(RadioRxArm::armed(p, false));
    TEST_ASSERT_FALSE_MESSAGE(RadioRxArm::armed(p, true),
                              "no duty-cycle call was made, so nothing can have accepted one");
  }
}

// The pair of published read-backs comes from this one rule asked twice — once
// as configured, once with the switch forced on — which is what keeps
// "this channel cannot" apart from "nobody asked" without a second copy of the
// condition. Pinned because a surface built on the narrow answer alone told an
// SX1262 at SF10/125 kHz that its channel was hopeless when the switch was the
// only thing missing.
static void test_the_switch_is_the_only_difference_between_the_two_read_backs() {
  for (int capable = 0; capable <= 1; capable++) {
    for (int engages = 0; engages <= 1; engages++) {
      const bool wouldEngage =
          RadioRxArm::dutyCycled(RadioRxArm::decide(true, capable != 0, engages != 0));
      const bool engagesOff =
          RadioRxArm::dutyCycled(RadioRxArm::decide(false, capable != 0, engages != 0));
      const bool engagesOn =
          RadioRxArm::dutyCycled(RadioRxArm::decide(true, capable != 0, engages != 0));
      TEST_ASSERT_FALSE_MESSAGE(engagesOff, "with the switch off nothing engages");
      TEST_ASSERT_EQUAL_MESSAGE(wouldEngage, engagesOn,
                                "with the switch on the two answers are the same question");
      TEST_ASSERT_EQUAL_MESSAGE(capable && engages, wouldEngage,
                                "would-engage is the chip and the channel, and only those");
    }
  }
}

// --- which preamble the window is sized on ----------------------------------

// The defect this rule exists for. startReceiveDutyCycleAuto()'s first argument
// is the *sender's* preamble, and the sleep is `sender - 2 * minSymbols`
// symbols long. Sizing that on radio.preamble — a local setting this firmware
// accepts from 6 to 1000 symbols — sleeps through a window no peer ever agreed
// to fill: at 64 symbols the receiver dozes for 48 symbol times while a
// conforming RNode-lineage peer sends 18, so its whole preamble fits inside the
// window and the packet is never heard, on a node reporting the mode healthy.
static void test_a_local_preamble_above_the_floor_sizes_on_the_floor() {
  static const uint16_t kAbove[] = { 19, 24, 32, 64, 128, 516, 1000 };
  for (size_t i = 0; i < sizeof(kAbove) / sizeof(kAbove[0]); i++) {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(
        RF_PREAMBLE_SYMS, RadioRxArm::sizingPreamble(kAbove[i], RF_PREAMBLE_SYMS),
        "a longer local preamble must not lengthen the sleep: peers are only "
        "obliged to send the floor");
  }
  // Exactly at the floor is the shipped node, and nothing changes for it.
  TEST_ASSERT_EQUAL_UINT16(RF_PREAMBLE_SYMS,
                           RadioRxArm::sizingPreamble(RF_PREAMBLE_SYMS, RF_PREAMBLE_SYMS));
}

// The other direction, and it is not symmetry for its own sake: RadioLib
// refuses a sender preamble longer than the configured one outright
// (RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH, PhysicalLayer.cpp), so a node
// configured below the floor cannot be handed the floor — it would arm nothing.
// The shorter window is also the honest one to sleep for, and such a node is
// outside the interop guarantee anyway.
static void test_a_local_preamble_below_the_floor_sizes_on_itself() {
  for (uint16_t cfg = 6; cfg < RF_PREAMBLE_SYMS; cfg++)
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(cfg, RadioRxArm::sizingPreamble(cfg, RF_PREAMBLE_SYMS),
                                     "the driver refuses a sender preamble longer than the "
                                     "configured one, so the floor cannot be used here");
}

// The two properties that make the choice safe, over the whole settings space
// the validator accepts (SettingsRules.h: 6-1000 symbols). Never above the
// configured preamble, or the arm call is refused and the receiver is left
// unarmed; never above the floor, or the sleep can swallow a conforming
// preamble.
static void test_the_sizing_preamble_never_exceeds_either_bound() {
  for (uint16_t cfg = 6; cfg <= 1000; cfg++) {
    const uint16_t sized = RadioRxArm::sizingPreamble(cfg, RF_PREAMBLE_SYMS);
    TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(cfg, sized,
        "a sender preamble past the configured one is refused before any arithmetic runs");
    TEST_ASSERT_LESS_OR_EQUAL_UINT16_MESSAGE(RF_PREAMBLE_SYMS, sized,
        "sleeping past the network floor is the missed-packet failure itself");
  }
}

// And the floor itself, restated: RF_PREAMBLE_SYMS is RNode's
// LORA_PREAMBLE_SYMBOLS_MIN, the number every conforming sender on this network
// respects. Changing it changes how long every duty-cycled node sleeps, so it
// does not get to move quietly.
static void test_the_floor_is_the_rnode_preamble_minimum() {
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(18, (uint16_t)RF_PREAMBLE_SYMS,
                                   "RF_PREAMBLE_SYMS is RNode's LORA_PREAMBLE_SYMBOLS_MIN "
                                   "and the duty-cycle window is sized on it");
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_switch_off_means_continuous_whatever_else_is_true);
  RUN_TEST(test_a_chip_without_the_mode_stays_continuous_and_says_so);
  RUN_TEST(test_a_channel_the_driver_will_not_sleep_on_is_not_offered_to_it);
  RUN_TEST(test_all_three_agreeing_is_the_only_way_to_the_duty_cycled_arm);
  RUN_TEST(test_a_refused_arm_is_not_an_armed_receiver);
  RUN_TEST(test_the_continuous_plans_can_never_report_armed);
  RUN_TEST(test_the_switch_is_the_only_difference_between_the_two_read_backs);
  RUN_TEST(test_a_local_preamble_above_the_floor_sizes_on_the_floor);
  RUN_TEST(test_a_local_preamble_below_the_floor_sizes_on_itself);
  RUN_TEST(test_the_sizing_preamble_never_exceeds_either_bound);
  RUN_TEST(test_the_floor_is_the_rnode_preamble_minimum);
  return UNITY_END();
}
