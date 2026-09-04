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
//  Watchdog.h — a task that stops making progress restarts the node
//
//  The gap this closes: ESP-IDF's task watchdog is enabled and set to panic,
//  but it watches only the idle task. Our tasks — the radio, the Reticulum
//  loop, the panel, the card — subscribe to nothing, so any one of them can
//  block for ever on a mutex, a bus or a socket while the idle task runs on
//  happily and the watchdog stays silent.
//
//  That is not hypothetical. A node in a yard went silent on LoRa and Wi-Fi at
//  the same moment with half a battery left, stayed silent until it was
//  carried indoors and power-cycled, and came back reporting a clean
//  power-on — no panic, no watchdog, nothing to read afterwards. A hang is the
//  one failure that costs a visit rather than a reboot, and it is the one
//  failure the node could not report or recover from.
//
//  So the loops that must keep running say so, and a loop that stops saying so
//  reboots the node. A reboot costs seconds. It also turns an invisible
//  failure into a countable one: Diag already names ESP_RST_TASK_WDT as a boot
//  reason and has never had a producer for it.
//
//  Choosing the timeout
//  --------------------
//  One timeout covers every subscribed task, so it has to clear the slowest
//  legitimate pass by a wide margin — a false trip in a field is a reboot loop
//  on a mast, which is worse than the hang it was meant to catch. The slow
//  ones are measured, not guessed: an e-paper full refresh is seconds, a
//  Reticulum pass that writes its store and verifies a signature is seconds,
//  and an SD format is minutes and is therefore not covered by a timeout at
//  all but by pause()/resume() around it.
//
//  WATCHDOG_TIMEOUT_S is set well above all of those. It is not tuned to catch
//  a hang quickly; it is tuned never to fire on a node that is working. A hang
//  discovered in half a minute instead of half a second is the same rescue.
// ============================================================================
#pragma once

#include <stdint.h>

namespace Watchdog {

// Called once from setup(), before any task subscribes. Widens IDF's default
// 5-second timeout, which several of our passes legitimately exceed.
void begin();

// Subscribe the calling task. Call from inside the task, not from whoever
// started it: the subscription is per-task and the caller is a different one.
void watch();

// "Still going." Call once per pass, at the top of the loop.
void feed();

// For work that is legitimately longer than the timeout and cannot be broken
// up — formatting a card, chiefly. The task is unsubscribed for the duration
// and re-subscribed after, so a hang *inside* the long operation is not
// caught; that is the trade, and it is why the list of callers is short.
void pause();
void resume();

// Whether begin() got the watchdog configured. False means the node is running
// unsupervised, which is worth saying out loud rather than assuming.
bool armed();

}  // namespace Watchdog
