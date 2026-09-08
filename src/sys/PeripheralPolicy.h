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
//  the twelve boards in boards.json; both calls are guarded, so what the other
//  ten pay is 160 bytes of flash, no RAM at all and a spinlock taken on a
//  screen edge — measured on heltec-v3, which carries neither part and is the
//  tightest board here. (An earlier version of this text said nine boards and
//  seven; both were wrong, and so was the byte figure quoted with them.)
//
//  The rule
//  --------
//  A sensor runs while the screen is lit and is suspended while it is dark —
//  where the screen is what reads it. That qualifier used to be true of both
//  parts on both boards that had them, and so went unsaid; the T-Beam Supreme
//  is where it stopped being true, and imuRuns() takes it as an argument now.
//  It is the same rule in every profile either way:
//
//   * the magnetometer is read for a heading on the glass and for a console
//     line, and its hard-iron calibration is captured from a board being
//     turned by hand — which is something a person does while looking at it.
//     Dark, it converts continuously for nobody;
//   * the accelerometer answers two questions and each board asks only one.
//     Which way up the panel is being held, which a dark panel does not care
//     about; or where level went, for the magnetometer's tilt correction,
//     which is suspended alongside it. Either consumer is the lit screen.
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

  // The rule itself, as two pure questions over the node's state. `profile`
  // is Power::Profile's ordinal, passed rather than interpreted — see why the
  // profile moves neither, above. Two functions and not one because they are
  // two different questions that happen to share an answer today, and the day
  // one of them parts company there is no call site to go hunting for.
  static constexpr bool compassRuns(bool screenDark, uint8_t profile) {
    (void)profile;
    return !screenDark;
  }
  // `screenConsumes` is the board fact that decides whether the screen going
  // dark says anything about this part at all: true where the panel turns
  // itself from the accelerometer, or where a magnetometer beside it needs
  // gravity for its tilt correction — and that magnetometer is suspended with
  // the screen, so its demand for gravity goes with it.
  //
  // False is the T-Beam Supreme, and it is why this rule and the one above
  // have parted company. That board has an accelerometer on the card's SPI
  // bus, no magnetometer at all, and a fixed 1.3" panel that does not rotate.
  // Its only readers are the console and the status API, which answer while
  // the glass is dark and are most often asked then. Suspending the part there
  // would have a fitted sensor report "asleep" to every caller it has — a
  // feature switched off by a rule written for boards where it made sense.
  static constexpr bool imuRuns(bool screenDark, uint8_t profile, bool screenConsumes) {
    (void)profile;
    return screenConsumes ? !screenDark : true;
  }

  // One event: the node's state as it now stands, and what that moves. The
  // caller passes the whole state rather than the edge, so a broadcast raised
  // for one input re-checks the other and no subscriber can be left holding a
  // verdict from a state that has since changed.
  //
  // That each field is fed from its own rule was, for a while, not observable
  // by any test: compassRuns() and imuRuns() computed the same boolean, and
  // swapping the two lines below changed nothing. They have since parted
  // company — imuRuns() takes a board fact the compass has no equivalent of —
  // so a swap fails a test now, which is what the earlier note said would
  // happen on the day it began to matter.
  Change update(bool screenDark, uint8_t profile, bool imuScreenConsumes) {
    Change c;
    const bool compass = compassRuns(screenDark, profile);
    if (compass != _compass) {
      _compass = compass;
      c.compass = compass ? Verdict::Run : Verdict::Suspend;
    }
    const bool imu = imuRuns(screenDark, profile, imuScreenConsumes);
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
