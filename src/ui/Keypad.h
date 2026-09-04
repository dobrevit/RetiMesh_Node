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
//  Keypad.h — keys the operator actually presses
//
//  Until this board every screen here was driven by one button: short press
//  turns the page, long press blanks the panel. That is the whole language a
//  gateway needs, and the LVGL shell added a touch keyboard on the glass for
//  the one thing it could not say — text. This board has real keys, so the
//  glass keyboard is no longer the only way to type, and the shell hides it.
//
//  Two sources of keys, one stream out
//  -----------------------------------
//  The keyboard is not a matrix this firmware scans. A microcontroller of its
//  own scans it and answers on I2C with one key per read — a latch, not a
//  held state, so there is no release event and nothing to debounce here.
//
//  The trackball is four GPIOs that pulse as the ball turns, one edge per
//  detent, plus a click that is the board's BOOT pin and therefore the same
//  pin PIN_BUTTON already watches. The edges are counted in an interrupt
//  because a poll at the display's rate would miss most of a quick roll.
//
//  Both arrive through read() as one stream of key codes, because that is what
//  every consumer wants: the shell feeds it to LVGL as a keypad device, and a
//  trackball detent is an arrow key by any sensible reading. The click is left
//  to the button, which already means "select" everywhere in this firmware.
//
//  Codes are ASCII where ASCII has an answer, and above it where it does not.
//  Deliberately not LVGL's constants: the mono page stack has no LVGL in the
//  build at all, and this has to compile there too.
// ============================================================================
#pragma once

#include "Config.h"
#include <stdint.h>

namespace Keypad {

enum : uint8_t {
  KEY_NONE      = 0x00,
  KEY_BACKSPACE = 0x08,
  KEY_ENTER     = 0x0D,
  KEY_ESC       = 0x1B,
  // Navigation, past the printable range so nothing collides with a character
  // the keyboard can send.
  KEY_UP        = 0x80,
  KEY_DOWN      = 0x81,
  KEY_LEFT      = 0x82,
  KEY_RIGHT     = 0x83,
};

#if HAS_KEYPAD || HAS_TRACKBALL

// Find the keyboard and arm the trackball. Cheap, and safe to call before the
// bus has any other user — it brings the shared bus up if nobody has yet.
void begin();

// The next key, or KEY_NONE. One call returns one key: the keyboard hands over
// one per read by construction, and a trackball roll is drained a detent at a
// time so a fast spin becomes several keys rather than one large jump.
uint8_t read();

// Whether the keyboard answered at boot. False on a board that has one fitted
// but not talking, which is worth showing rather than hiding — the shell keeps
// its on-glass keyboard when this is false, so the node stays usable.
bool present();

#else

inline void begin() {}
inline uint8_t read() { return KEY_NONE; }
inline bool present() { return false; }

#endif

} // namespace Keypad
