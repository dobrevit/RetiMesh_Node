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
//  TouchInput.h — the capacitive layer over the panel, as a button
//
//  The pages are driven by one button: short press turns the page, long press
//  blanks the panel. A touch layer could carry a whole gesture language, but
//  inventing one for five status pages buys nothing a tap does not — so the
//  layer is read as a second button, and the display task treats a tap
//  exactly as a short press and a long hold exactly as a long one. Anyone
//  who has used the button knows the screen, and the day the UI grows
//  something worth pointing at, poll() already returns coordinates.
//
//  Polled, not interrupt-driven. The board's published map has the interrupt
//  line commented out and nobody has confirmed the wire exists; a poll a few
//  times a second costs one short I2C transaction against a controller that
//  answers only while touched, and cannot be wrong about the wiring.
//
//  The controller answers on its own I2C pair, away from every other bus, and
//  only while a finger is down — an idle poll is a failed read, which is the
//  cheap and expected case, not an error.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_TOUCH

#include <stdint.h>

namespace TouchInput {

// One reading. `down` is "a finger is on the glass now"; x and y are in the
// panel's coordinates when it is.
struct Point {
  bool    down = false;
  int16_t x = 0;
  int16_t y = 0;
};

// Bring up the controller's bus and release its reset. Cheap; called once
// from the display's begin, since input for a panel nobody can see is not
// worth a bus.
void begin();

Point poll();

} // namespace TouchInput

#else

namespace TouchInput {
struct Point { bool down = false; int16_t x = 0; int16_t y = 0; };
inline void begin() {}
inline Point poll() { return Point{}; }
} // namespace TouchInput

#endif // HAS_TOUCH
