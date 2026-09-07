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
//  DisplayPace.h — how long the display task may sleep between passes
//
//  The GUI shell says when it wants running again: lv_timer_handler() returns
//  the time until its next timer is due, and LvglUi::loop() hands that figure
//  straight back. The display task threw it away and slept BUTTON_POLL_MS
//  regardless — fifty passes a second on a screen showing a clock nobody is
//  reading, on the one task that has nothing urgent of its own.
//
//  It cannot simply be obeyed either, for two reasons. The shell answers
//  0xFFFFFFFF when no timer is pending at all, which is seven weeks; and the
//  pass is not only the shell's. In the same loop the task polls the button,
//  writes the backlight level, walks the idle/blank staging, tells the
//  peripherals what the screen is doing and feeds the watchdog. So the
//  shell's answer is a request, and this file is the bound put on it.
//
//  The floor: the pass the task has always run
//  -------------------------------------------
//  BUTTON_POLL_MS. Not a new number, deliberately: it is the cadence every
//  one of those duties was written against, and taking it as the floor means
//  a busy screen — an animation, a scroll, a keyboard being typed on — runs
//  exactly the loop it ran before rather than spinning the task at the tick
//  rate for a shell asking to be called back in one millisecond. LVGL renders
//  at a 33 ms refresh period, so a 20 ms floor already offers it more passes
//  than its own frames need. The change only ever lengthens a pass.
//
//  The ceiling: what the button needs
//  ----------------------------------
//  The co-tenant duties are not equally urgent. The watchdog wants a feed
//  inside WATCHDOG_TIMEOUT_S — thirty seconds. The idle and blank timers are
//  minutes long. The backlight level follows a setting somebody has just
//  changed and the accelerometer is read once a second. The button is the
//  tight one, and it decides the ceiling, because a press that is missed or
//  answered late is a node that feels dead in the hand.
//
//  The press grammar (Display.h, PressTracker) spends three passes on a tap
//  in the worst case: one to sample a finger that is down — a press shorter
//  than a pass can be missed entirely — and two more to prove it has lifted,
//  since a single dropped report mid-hold must not read as a release. A
//  hundred milliseconds is the span in which a person reads a response as
//  instantaneous, so three passes have to fit inside it and the ceiling is
//  a third of it.
//
//  That lands on 33 ms, which is also what the shell asks for while it idles
//  — its display refresh and input read timers both run at LVGL's 33 ms
//  period. The agreement is not a coincidence to lean on but it is a useful
//  one: today the shell's request passes through this rule untouched, and
//  the display task drops from 50 passes a second to 30 on a lit screen with
//  nothing happening. The ceiling is what stops that becoming seven weeks if
//  the shell's timers ever go away.
//
//  What it does not pace
//  ---------------------
//  A blanked panel and a board with no shell never call this: the task keeps
//  the floor there, because on those paths the button and the wake poll are
//  the whole pass and there is no request to honour. That is what keeps the
//  wake-from-blank budget where the backlight work measured it — the wake
//  poll's quarter second plus one floor-length pass — rather than adding the
//  ceiling to it. Even if that path were ever paced by this rule, the
//  ceiling keeps the sum inside the same budget, which is the second thing
//  the number above is chosen to protect.
//
//  Pure — no Arduino, no LVGL, no clock — so the bounds are pinned on the
//  host (test/test_display_pace) rather than by watching a panel and
//  wondering whether that press was slow. Config.h comes in for the two
//  numbers this shares with the task it paces, and nothing else.
// ============================================================================
#pragma once

#include <stdint.h>
#include "Config.h"

namespace DisplayPace {

// The pass the display task has always run, and the shortest it may run now.
constexpr uint32_t kMinMs = BUTTON_POLL_MS;

// The span a person reads as immediate, and the number of passes the press
// grammar spends inside it. Written as the derivation rather than as its
// answer, so that a retune of the grammar — a third "up" poll, say — moves
// the ceiling with it instead of leaving it stale.
constexpr uint32_t kResponseMs   = 100;
constexpr uint32_t kPassesPerTap = 3;

// The longest the task may sleep between passes.
constexpr uint32_t kMaxMs = kResponseMs / kPassesPerTap;

// The whole rule: what the shell asked for, bounded by what the rest of the
// pass needs. Total over every uint32_t, including the 0xFFFFFFFF the shell
// answers when it has no timer pending and the 0 it answers when one is
// already due.
constexpr uint32_t passMs(uint32_t requestedMs) {
  if (requestedMs < kMinMs) return kMinMs;
  return requestedMs > kMaxMs ? kMaxMs : requestedMs;
}

// A floor above the ceiling would make passMs() answer the floor for every
// request, which is the old fixed pass wearing this file's name — so it is a
// build failure rather than a silent loss of the saving.
static_assert(kMinMs <= kMaxMs, "the pass floor cannot sit above the ceiling");
static_assert(kMinMs >= 1, "a floor of zero is a spinning task, not a pass");

// The task feeds the watchdog once per pass, so this ceiling is also the
// longest a healthy pass may go without reporting — before the pass's own
// work is counted. A hundredth of the timeout leaves all of the rest to that
// work, which is what WATCHDOG_TIMEOUT_S was sized for (Config.h: a panel's
// full refresh is seconds).
static_assert(kMaxMs * 100 <= (uint32_t)WATCHDOG_TIMEOUT_S * 1000UL,
              "the pass ceiling must leave the watchdog its margin");

} // namespace DisplayPace
