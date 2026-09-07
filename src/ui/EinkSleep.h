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
//  EinkSleep.h — the order an e-paper controller has to be asked in
//
//  The controller behind the 2.13" panel has a deep sleep worth having and
//  exactly one way out of it, and the way out is not a command. From the
//  panel's command table:
//
//    R10h "Deep Sleep mode": A[1:0] — 00 Normal Mode [POR], 01 Enter Deep
//    Sleep Mode 1, 11 Enter Deep Sleep Mode 2. "After this command initiated,
//    the chip will enter Deep Sleep Mode, BUSY pad will keep output high.
//    Remark: To Exit Deep Sleep mode, User required to send HWRESET to the
//    driver."
//
//  And the software reset is explicitly not a way out — R12h "SWRESET: It
//  resets the commands and parameters to their S/W Reset default values
//  *except R10h-Deep Sleep Mode*". So a sleeping controller ignores every
//  command that follows until its reset line has been pulled, and a driver
//  that does not know it put the part to sleep will write a frame into
//  nothing and wait on a BUSY line that never falls.
//
//  Second constraint, and the one that decides what the first update after a
//  wake has to be. The partial update this panel spends most of its life on
//  is differential: the driver leaves the frame it just showed in the
//  controller's second RAM so the next update can be computed against it.
//  Whether that RAM survives deep sleep is *not established* — the panel is
//  a HINK-E0213A162-A1 and no datasheet for it ships with the driver library;
//  the sibling part whose datasheet does (a DKE 2.13" on the same command
//  set) quotes its deep-sleep current with "Ram data not retain" against a
//  plain sleep row that says "Ram data retain", and its command table names
//  two deep-sleep modes without saying which is which. So this file assumes
//  the reference is gone. A full refresh rewrites both RAMs from scratch and
//  is correct under either answer; a partial one is correct under only the
//  favourable answer, and being wrong about it means ghosting that lasts
//  until the next full refresh comes round.
//
//  Hence the whole rule, which is three sentences:
//
//    * asking a sleeping controller to sleep again sends nothing, and asking
//      a waking one to wake sends nothing — both events arrive repeatedly
//      (the screen edge is reached from the idle timer, a long press and the
//      page walker) and the controller must be written to once per change;
//    * the update after a wake reloads the controller's mode, which is how
//      this driver is asked for the hardware reset, and is a full refresh;
//    * an update that arrives while the controller is asleep is a wake. A
//      boot notice painted straight to the glass does not go through the
//      screen-state edge, and it must not be the frame that discovers the
//      part is not listening.
//
//  Pure — no Arduino, no driver, no clock — so the sequence is pinned on the
//  host (test/test_eink_sleep) rather than by watching a panel. The register
//  writes it orders are bench-only and are not faked here.
// ============================================================================
#pragma once

#include <stdint.h>

class EinkSleep {
public:
  // What the panel owes the controller before the update it is about to run.
  // Both false is the ordinary case and costs nothing.
  struct Preamble {
    bool reload = false;    // reload the mode, which is this driver's reset
    bool full   = false;    // and show a whole frame rather than a difference
  };

  // True when the deep-sleep command has to be sent. False means the
  // controller is already asleep and the caller sends nothing.
  bool sleep() {
    if (_asleep) return false;
    _asleep = true;
    return true;
  }

  // True when this actually moved the state. Nothing is sent here: the part
  // only leaves deep sleep on a hardware reset, and the reset belongs to the
  // update that needs it — waking a panel nobody then draws to would pay the
  // reset and the mode reload for a frame that never comes.
  bool wake() {
    if (!_asleep) return false;
    _asleep = false;
    _resume = true;
    return true;
  }

  // The update is about to run; `wantFull` is what the refresh policy asked
  // for. A resume overrides it upwards and never downwards.
  Preamble beforeUpdate(bool wantFull) {
    // An update with no wake before it. Whoever drew this frame did not come
    // through the screen-state edge, and the controller is still deaf.
    if (_asleep) { _asleep = false; _resume = true; }
    Preamble p;
    p.full = wantFull;
    if (_resume) {
      _resume = false;
      p.reload = true;
      p.full   = true;
    }
    return p;
  }

  // For the tests, and for reading a log next to a panel that did not answer.
  bool asleep() const { return _asleep; }
  bool resuming() const { return _resume; }

private:
  // Both false at construction: begin() resets the panel and shows a frame,
  // so a freshly started controller is awake and owes nothing.
  bool _asleep = false;
  bool _resume = false;
};
