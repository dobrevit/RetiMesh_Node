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
//  PeripheralPolicy.h — which parts follow the screen, and which the profile
//
//  Two things change what a peripheral off the data path ought to be doing:
//  the screen going dark, and the power profile being switched. Neither used
//  to reach anything. Display::setBlank() told LVGL and the panel; Power::
//  apply() told the CPU clock and the Wi-Fi driver; and the parts that were
//  configured into their hottest mode at boot went on running in it for ever,
//  converting for a screen nobody was looking at.
//
//  The rule for which part follows which lives here, once, and Power's
//  broadcast (Power::onScreenBlank) asks it. Two subsystems have subscribed so
//  far, the magnetometer and the accelerometer, fitted between them on two of
//  the thirteen boards in boards.json; both calls are guarded, so what the
//  other eleven pay is 160 bytes of flash, no RAM at all and a spinlock taken
//  on a screen edge — measured on heltec-v3, which carries neither part and is
//  the tightest board here. (An earlier version of this text said nine boards
//  and seven, then twelve and ten; the figure that matters is that a board
//  with neither part pays nothing measurable.)
//
//  The rule
//  --------
//  A sensor runs while the screen is lit and is suspended while it is dark —
//  *on the boards that say so*. That qualifier used to be no qualifier at all:
//  both parts on both boards that had them behaved this way, so the rule was
//  written as if the screen were their only consumer. It is not, and on one
//  board it never was:
//
//   * a heading is read by the console and the status API, and by no page on
//     any board here — nothing in src/ui asks for one. Its hard-iron
//     calibration is captured from a board being turned by hand, which is
//     something a person does while looking at the node, but that is an
//     argument about calibration and not about who reads the number;
//   * the accelerometer answers two questions and each board asks at most one.
//     Which way up the panel is being held, which a dark panel does not care
//     about; or where level went, for a magnetometer's tilt correction — and
//     that one holds only while the magnetometer is itself suspended with the
//     screen, because a compass that keeps converting in the dark keeps
//     wanting gravity.
//
//  So each part takes its board's answer as an argument, and the answer is a
//  power decision rather than a claim about consumers: the M9 suspends both
//  because it is a handheld on a cell that blanks every twenty seconds and has
//  always done so; the T-Beam Supreme suspends neither because it is a gateway
//  whose readers are asked of a dark node (Config.h states both, with the
//  standby current the M9's value is really waiting on).
//
//  Why the profile moves neither
//  -----------------------------
//  The profile is an input to this rule and deliberately does not change it.
//  A lit screen on a battery-saving handheld is somebody holding the node and
//  reading it: suspending the compass there would not make the profile
//  cheaper, it would make the heading wrong, and a profile that quietly breaks
//  a feature is worse than one that saves a milliamp less. What a profile
//  could honestly buy on these two parts is a slower conversion rate while
//  lit — and that is a register value per part which has to come off a
//  datasheet and be confirmed against the hardware, not be guessed in a
//  policy header. Until it is, the answer here is invariant in the profile,
//  and the test says so rather than leaving it to be inferred.
//
//  The broadcast carries profile changes all the same, and that is not
//  decoration: it is what keeps this file the only place the rule lives.
//  Power::apply() re-asks, gets the same answer, and writes nothing. The
//  alternative — wiring the profile in on the day it first matters — is a
//  second call site to find and a peripheral state that only catches up when
//  the screen happens to move.
//
//  Why it remembers
//  ----------------
//  Because the events repeat and the drivers must not be written to twice.
//  setBlank(true) is reached from the idle timer, the power menu, a long
//  press and the deep-sleep path, and several of those can arrive with the
//  panel already dark; a profile can be re-applied with nothing changed at
//  all (the settings commit does it unconditionally). So the verdicts are
//  latched and an update() answers with what *moved*, which for most events
//  is nothing. The drivers are idempotent too — they have to be, they are
//  told from a different task than the one that owns them — but the cheapest
//  bus transaction is the one nobody asks for.
//
//  The verdicts start at "running", because that is the state begin() leaves
//  both parts in and the screen is lit when it does. So the first ask after
//  boot moves nothing, which is right: nothing needs saying.
//
//  Pure — no Arduino, no FreeRTOS, no clock, no timestamps — so the routing
//  is unit-tested on the host (test/test_peripheral_policy) instead of being
//  watched on a bench with a current probe. One caller, one broadcast: not
//  synchronised, like SampleGate and ApIdlePolicy.
// ============================================================================
#pragma once

#include <stdint.h>

class PeripheralPolicy {
public:
  // What one subsystem is told by an event, if anything. Unchanged is the
  // usual answer and the reason this type is not a bool.
  enum class Verdict : uint8_t { Unchanged, Run, Suspend };

  // What one event moved. Both fields Unchanged means the broadcast has
  // nothing to say, which is the common case and costs no transactions.
  struct Change {
    Verdict compass = Verdict::Unchanged;
    Verdict imu     = Verdict::Unchanged;
  };

  // The rule itself, as two pure questions over the node's state, and it is
  // one sentence: a part follows the screen only where the screen is what
  // reads it. `profile` is Power::Profile's ordinal, passed rather than
  // interpreted — see why the profile moves neither, above.
  //
  // `screenConsumes` is the board fact each question turns on, and it is a
  // different fact for each part, which is why there are two functions and not
  // one. For the accelerometer it is true where the panel turns itself from
  // the part, or where a magnetometer beside it needs gravity to level a
  // heading *and is itself suspended with the screen* — that last clause is
  // what makes the demand for gravity go dark with the glass. For the
  // magnetometer it is true where a page shows a bearing.
  //
  // False for both is the T-Beam Supreme. That board puts its accelerometer on
  // the card's SPI bus, drives a magnetometer no page displays, and has a
  // fixed 1.3" panel that does not rotate. The readers are the console and the
  // status API, which answer while the glass is dark and are most often asked
  // then, so suspending either part there would hand "asleep" to every caller
  // it has — a feature switched off by a rule written for boards where it made
  // sense.
  //
  // The two facts are not independent, and Config.h is where that is said
  // once: a compass that keeps running in the dark holds the accelerometer up
  // with it, because it still wants gravity. Nothing in here knows which board
  // it is; a call site that passed the compass's fact to the accelerometer's
  // question would compile and be wrong, which is what the tests below are
  // for.
  // One rule, asked twice. The two questions keep their own names because they
  // are asked of different parts with different board facts; what they share
  // is the sentence, and it is written once so that a change to it cannot land
  // on one part and not the other.
  static constexpr bool followsScreen(bool screenDark, bool screenConsumes) {
    return screenConsumes ? !screenDark : true;
  }
  static constexpr bool compassRuns(bool screenDark, uint8_t profile,
                                    bool screenConsumes) {
    (void)profile;
    return followsScreen(screenDark, screenConsumes);
  }
  static constexpr bool imuRuns(bool screenDark, uint8_t profile, bool screenConsumes) {
    (void)profile;
    return followsScreen(screenDark, screenConsumes);
  }

  // The two board facts, as two types rather than two bools. They are adjacent
  // arguments of the same type with different meanings, there is one call site,
  // and on both boards that exist today they happen to be equal — so a swap
  // would change nothing observable and no test could catch it. An earlier
  // version of this comment claimed the tests held the pairing. They cannot;
  // these do, because swapping them does not compile.
  struct ImuFollowsScreen     { bool value; };
  struct CompassFollowsScreen { bool value; };

  // One event: the node's state as it now stands, and what that moves. The
  // caller passes the whole state rather than the edge, so a broadcast raised
  // for one input re-checks the other and no subscriber can be left holding a
  // verdict from a state that has since changed.
  //
  // Each field is fed from its own rule with its own board fact. Getting the
  // two the wrong way round would suspend a compass on a node whose only
  // readers are dark, or keep an accelerometer awake for a heading nobody
  // displays — both silent, which is why the facts arrive as the two types
  // above rather than as two bools.
  Change update(bool screenDark, uint8_t profile, ImuFollowsScreen imuFollows,
                CompassFollowsScreen compassFollows) {
    Change c;
    const bool compass = compassRuns(screenDark, profile, compassFollows.value);
    if (compass != _compass) {
      _compass = compass;
      c.compass = compass ? Verdict::Run : Verdict::Suspend;
    }
    const bool imu = imuRuns(screenDark, profile, imuFollows.value);
    if (imu != _imu) {
      _imu = imu;
      c.imu = imu ? Verdict::Run : Verdict::Suspend;
    }
    return c;
  }

  // What the last update left each subsystem believing — for the tests, and
  // for anyone reading a log beside a driver that refused its write.
  bool compassRunning() const { return _compass; }
  bool imuRunning() const { return _imu; }

private:
  bool _compass = true;                  // both parts come up running: begin()
  bool _imu     = true;                  // configures them and the screen is lit
};
