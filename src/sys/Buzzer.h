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
//  Buzzer.h — the sounder, for the two things worth a sound
//
//  A piezo on a PWM pin, not an audio path: what it can do is a tone at a
//  frequency for a while. It says exactly two things. A short rising pair at
//  boot — the node is up, worth having on a device whose panel takes a moment
//  to say the same. And a single higher note when a message for this node
//  arrives, because a handheld with a screen is a thing somebody carries, and
//  a message that arrives silently on a carried device was not delivered in
//  any sense the carrier noticed.
//
//  Nothing here blocks: a note is started and a one-shot timer ends it, so
//  the caller — which on arrival is the RNS task — pays a register write, not
//  the length of the note. Notes do not queue; a sound that arrives while one
//  is playing is the same news, and the news is already being said.
//
//  Boards without a sounder compile all of it away.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_BUZZER

namespace Buzzer {

void begin();     // claims what the board needs, if the sounder is switched on
void boot();      // two short notes up: running
void message();   // one note: something arrived for you

// Take up or release the hardware to match the setting, without a restart.
// That is the point of the switch on the speaker boards: the task and the DMA
// ring are about 5.4 KB of internal RAM, a quarter of what one of these boards
// has spare and enough to stop the portal serving — so turning the sounder off
// has to give that back rather than merely stay quiet.
void apply();

// Whether the sounder is up and can be heard. False when the board has none,
// when it is switched off, and when it would not start.
bool present();

} // namespace Buzzer

#else

namespace Buzzer {
inline void begin() {}
inline void boot() {}
inline void message() {}
inline void apply() {}
inline bool present() { return false; }
} // namespace Buzzer

#endif // HAS_BUZZER
