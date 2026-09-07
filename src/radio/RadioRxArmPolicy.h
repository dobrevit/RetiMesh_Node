// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

// ============================================================================
//  RadioRxArmPolicy.h — which receive the radio arms, and what it may claim
//
//  Three separate things decide whether the receiver sleeps between preamble
//  samples: the operator asked for it (the radio.rx_duty_cycle setting), the
//  fitted transceiver has the mode (RadioCaps::Caps::rxDutyCycle, true only for
//  the SX1262), and the channel yields a sleep the driver will actually take
//  (Airtime::rxDutyCycleEngages). All three are pure values by the time the
//  radio has them, so the decision is written here as one rule rather than as
//  a condition repeated at each of the dozen places that re-arm the receiver.
//
//  Repeated is exactly what it would have been. The chip drops out of receive
//  on every reception, every transmission and every channel-activity probe, so
//  the driver is told to receive again from eleven places in LoRaRadio.cpp. A
//  decision made at one of them and not the others is not a partial feature: it
//  is a feature that switches itself off the first time a packet arrives, while
//  every surface goes on reporting it as on.
//
//  ---------------------------------------------------------------------------
//  Why a channel the mode does not suit arms a plain continuous receive here,
//  rather than asking the driver and letting it fall back
//  ---------------------------------------------------------------------------
//  RadioLib will accept the call on such a channel and quietly do something
//  else, and there are two different something-elses behind one false from
//  Airtime::rxDutyCycleEngages():
//
//    * the sleep is shorter than the chip's wake-up transition, so
//      startReceiveDutyCycleAuto() calls startReceive() itself and returns
//      RADIOLIB_ERR_NONE. Harmless on the air — the node hears everything —
//      but it returns success for a receiver that is running continuously.
//    * the sleep will not fit the 24 bits the SX1262's SetRxDutyCycle command
//      carries, so the driver returns RADIOLIB_ERR_INVALID_SLEEP_PERIOD from
//      *before* the stageMode() call that would have put the part into any
//      receive at all. The chip is left in standby and the node is deaf until
//      something else re-arms it.
//
//  The predicate deliberately does not tell those two apart (Airtime.h says
//  why), so asking the driver on a false is a coin toss between a harmless
//  no-op and a deaf node. That alone settles it. The second reason matters just
//  as much though, and it is about honesty rather than safety: the driver's own
//  fallback reports success, so a caller that published "armed" on a
//  RADIOLIB_ERR_NONE would claim the saving on precisely the channel where it
//  is not being made — which is the failure this whole feature was built to
//  avoid making. Deciding here keeps the claim answerable.
//
//  ---------------------------------------------------------------------------
//  Why armed() takes the driver's answer and not just the plan
//  ---------------------------------------------------------------------------
//  The plan is what the node intends; armed is what the chip accepted. They
//  come apart whenever the driver refuses a call the rule expected it to take —
//  a library bump moving a threshold, a chip that has stopped answering — and
//  the caller's contract in that case is to fall straight back to a continuous
//  receive, because a receiver left unarmed is a node that hears nothing at
//  all. `rxDutyCycleArmed` is the one field an operator reads to tell a working
//  feature from a silently inactive one, so it is derived from what the driver
//  said, never from what the setting or the prediction says.
//
//  ---------------------------------------------------------------------------
//  The fourth number, which is not part of the plan but belongs beside it
//  ---------------------------------------------------------------------------
//  Once the plan is DutyCycled the driver still has to be told how long a
//  preamble to expect *from other people*, and that is a different number from
//  this node's own preamble setting. sizingPreamble() below is that rule, kept
//  here because the prediction and the arm call must not each derive it.
//
//  Pure, so the whole table is a host test (test/test_radio_rx_arm) rather than
//  a bench session with an ammeter.
// ============================================================================
#pragma once

#include <stdint.h>

namespace RadioRxArm {

// What the next arm will be — and, where it is not the duty-cycled one, why.
// The reason is not decoration: it is what the apply-path log line says out
// loud, and a node reporting "off" when the truth is "this radio has no such
// mode" has sent its operator to the wrong dial.
enum class Plan : uint8_t {
  Continuous,         // the switch is off — nobody asked for the saving
  Unsupported,        // asked for, but this transceiver has no such mode
  ChannelUnsuitable,  // asked for and supported, but not on this channel
  DutyCycled,         // ...and on this channel the sleep is worth taking
};

// The rule. `channelEngages` is Airtime::rxDutyCycleEngages() for the channel
// being applied, `chipCapable` is RadioCaps::Caps::rxDutyCycle for the detected
// part, `settingOn` is radio.rx_duty_cycle.
//
// Order matters, because the answer names the first thing that is missing and
// the operator can only act on one dial at a time: the switch is theirs, the
// channel is theirs, the chip is not.
inline Plan decide(bool settingOn, bool chipCapable, bool channelEngages) {
  if (!settingOn)      return Plan::Continuous;
  if (!chipCapable)    return Plan::Unsupported;
  if (!channelEngages) return Plan::ChannelUnsuitable;
  return Plan::DutyCycled;
}

// Which preamble the sleep window is sized on — the one number both the
// prediction and the arm call have to agree about.
//
// startReceiveDutyCycleAuto()'s first argument is not this node's preamble. It
// is the *sender's*: "Expected preamble length of the messages to receive"
// (SX126x.h:333), and PhysicalLayer::calculateRxDutyCycle sleeps through
// `senderPreambleLength - 2 * minSymbols` symbols of it
// (PhysicalLayer.cpp:600). Handing it our own setting says "every peer
// transmits at least as long a preamble as I do", which is a claim about other
// people's radios that nothing entitles us to make: radio.preamble is an
// operator setting this firmware accepts anywhere from 6 to 1000 symbols
// (SettingsRules.h), while a conforming RNode-lineage peer is only ever
// obliged to send RF_PREAMBLE_SYMS. A node configured to 64 would sleep for
// 48 symbol times per cycle, and an 18-symbol preamble from a standard peer
// can fall wholly inside that window — the packet is simply not heard, on a
// node whose surfaces all report the mode working perfectly.
//
// So the window is sized on the floor the network guarantees, not on our own
// setting, and `floorSyms` is RF_PREAMBLE_SYMS: the minimum any conforming
// sender uses. Taking the smaller of the two is safe in both directions and is
// the whole rule:
//
//   * configured above the floor — 64, say — sizes on 18, which is the longest
//     window that still catches the shortest conforming preamble. Raising this
//     node's preamble lengthens what it transmits and changes nothing here.
//   * configured below the floor — 6, say — sizes on 6. It has to: the driver
//     refuses a sender preamble longer than the configured one with
//     RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH before it computes anything
//     (PhysicalLayer.cpp:588-590), so passing 18 there would arm nothing. The
//     shorter window is also the correct one to sleep for, and at 6 symbols it
//     floors to zero and the mode never engages at all — which is right, since
//     a node below the floor is already outside the interop guarantee.
//
// Both the published prediction and the arm call read this, once, so what the
// node reports is what it armed. Pure, so test_radio_rx_arm pins the rule and
// test_radio_duty_cycle pins it against the driver's own arithmetic.
inline uint16_t sizingPreamble(uint16_t configuredSyms, uint16_t floorSyms) {
  return configuredSyms < floorSyms ? configuredSyms : floorSyms;
}

// Whether that plan means startReceiveDutyCycleAuto() rather than
// startReceive(). Three of the four arm the same continuous receive; only the
// wording of the log line separates them.
inline bool dutyCycled(Plan p) { return p == Plan::DutyCycled; }

// What the node may publish as rxDutyCycleArmed once the driver has answered.
// `driverAccepted` is that answer — RADIOLIB_ERR_NONE from the duty-cycle call
// — and is meaningless for the other three plans, which never make one.
inline bool armed(Plan p, bool driverAccepted) {
  return dutyCycled(p) && driverAccepted;
}

} // namespace RadioRxArm
