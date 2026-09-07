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
//  BacklightLadder.h — how bright the glass is, given what it is showing
//
//  A backlit panel had one level: the operator's setting, written once per
//  change. The shell, though, rests in three states and not two — it works,
//  then it collapses to a clock, and only after four quiet timeouts does it
//  truly blank. The clock stage is the one this file exists for: on a
//  handheld it can last four minutes, nobody is reading it, and until now it
//  burned the full configured backlight for every second of that. The
//  backlight is the largest single draw on these boards, so a stage that is
//  glanced at rather than read should not cost what a stage being read costs.
//
//  The ladder, then, is a pure question — this many percent configured, in
//  this stage, under this profile, means this many percent at the glass —
//  and nothing else. It computes a *transient reduction*: the operator's
//  setting is never rewritten by it, and is always the ceiling. Turn the
//  brightness down and every rung goes down with it; turn it up and the
//  idle rung follows. What a percentage then does to hardware stays with
//  the panel, which is the only thing that knows whether it drives a duty
//  cycle or counts pulses into a one-wire dimmer.
//
//  The rule
//  --------
//    Active — the configured percentage, in every profile. Somebody is
//      reading this screen. A profile that dims what a person is looking at
//      buys nothing: they turn the brightness up, and the setting they leave
//      behind then costs more in every other stage too.
//    Idle   — the configured percentage stepped down, by more as the profile
//      gets thriftier (halved, quartered, eighthed). Floored so the clock
//      stays a clock: an unreadable idle screen is a blank screen that also
//      costs current, and darkness belongs to the blank stage rather than to
//      this one — the same sentence the settings funnel says about zero
//      (SettingsFields.cpp), which is why the floor here is that same
//      BRIGHTNESS_FLOOR_PCT and not a second number.
//    Blank  — off. Nothing is on the glass.
//
//  Why the profile moves this and not the sensor policy
//  ----------------------------------------------------
//  PeripheralPolicy.h deliberately answers the same in every profile, because
//  what a profile could honestly buy there is a register value off a
//  datasheet. Here the profile has something real and continuous to move: the
//  backlight is an analogue draw with no correctness attached to it. Dimmer
//  costs less and is harder to read, and how that trade should fall is
//  exactly what choosing a power profile says. The steps are monotone, so a
//  node moved to a thriftier profile never gets brighter.
//
//  Pure — no Arduino, no LVGL, no clock, no panel — so it is unit-tested on
//  the host (test/test_backlight_ladder) rather than judged by eye on a
//  bench. Config.h comes in for the one floor it shares with the settings
//  funnel, nothing else.
// ============================================================================
#pragma once

#include <stdint.h>
#include "Config.h"

namespace BacklightLadder {

// The stages the shell actually has, in the order it walks them. Named for
// what is on the glass, not for how long it has been there: the timings are
// LvglUi::restTick's business and this file must not grow a second opinion
// about them.
enum class Stage : uint8_t { Active = 0, Idle = 1, Blank = 2 };

// Which stage the glass is in, from the three facts that decide it. Here
// rather than in the display task because it is the other half of the same
// rule — a ladder whose caller picks the rung is a ladder with two owners —
// and because these three flags have a precedence a reader has to get right:
// a blanked panel is blank whatever else is true (the shell leaves its idle
// clock showing when a notice wakes the glass, and a board whose shell never
// started has no idle stage at all, only the working screen). Pure, so the
// eight combinations are pinned on the host instead of walked on a bench.
constexpr Stage stageOf(bool blank, bool shellUp, bool idleShowing) {
  if (blank) return Stage::Blank;
  return shellUp && idleShowing ? Stage::Idle : Stage::Active;
}

// How far the idle clock is stepped down, per Power::Profile ordinal, as a
// right shift of the configured percentage. Halved, quartered, eighthed:
// at the default 80 % that is 40 / 20 / 10 %, which brackets the reduction
// the power work was scoped around and keeps the thriftiest profile at the
// dimmest end. Written as shifts because the intent is "a fraction of what
// the operator asked for", and because a retune has to move a number in this
// table and show up in the diff rather than hide inside an expression.
constexpr uint8_t kProfileCount = 3;                       // Performance, Balanced, Battery
constexpr uint8_t kIdleShift[kProfileCount] = { 1, 2, 3 };

// The whole rule. `profile` is Power::Profile's ordinal, passed rather than
// interpreted — the same shape PeripheralPolicy uses, and for the same
// reason: this header stays free of Arduino code and of Power.h. A profile
// ordinal this file does not know falls back to Performance, the brightest
// and least surprising rung, rather than indexing off the end of the table.
constexpr uint8_t level(uint8_t configuredPct, Stage stage, uint8_t profile) {
  // The operator's setting is the ceiling of everything below it, so it is
  // clamped once, here, rather than trusted from a caller.
  const uint8_t ceiling = configuredPct > 100 ? (uint8_t)100 : configuredPct;
  if (stage == Stage::Blank) return 0;
  if (stage == Stage::Active) return ceiling;
  const uint8_t shift = kIdleShift[profile < kProfileCount ? profile : 0];
  const uint8_t stepped = (uint8_t)(ceiling >> shift);
  // Floored to stay readable, then held under the ceiling: an operator who
  // has already set the panel near its minimum has nothing left to give, and
  // the floor must not brighten the idle screen past the working one.
  const uint8_t lit = stepped < BRIGHTNESS_FLOOR_PCT ? (uint8_t)BRIGHTNESS_FLOOR_PCT : stepped;
  return lit > ceiling ? ceiling : lit;
}

} // namespace BacklightLadder
