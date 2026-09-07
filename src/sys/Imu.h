// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Imu.h — which way up the node is, and how hard gravity pulls
//
//  Two questions from the same six axes. Which edge of the panel points down,
//  so the display can follow the hand holding it — raw and unfiltered here,
//  because only the caller knows what a twitch costs on its glass. And the
//  acceleration itself, which is what turns a magnetometer into a compass: a
//  heading taken from magnetic field alone is only true while the board is
//  held level, and gravity is what says how far from level it is.
//
//  Two parts answer to this, named by IMU_KIND. They are not alike — one is a
//  bare accelerometer, the other a 6-axis part whose registers do not even
//  auto-increment until told to — and a board says which it carries rather
//  than anything here probing for it.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_IMU

#include <stdint.h>

namespace Imu {

enum class Facing : uint8_t { Up0, Up90, Up180, Up270, Flat, Unknown };

void begin();                            // probe and start the accelerometer
bool present();

Facing facing();                         // one fresh reading's verdict

// Gravity and whatever else is shaking the board, in g, as x, y, z. False when
// there is nothing to read. Unfiltered, like facing(): a compass wants a
// settled board and says so itself rather than being handed a smoothed lie.
bool accel(float g[3]);

// Put the accelerometer into its low-power state, or bring it back. A dark
// panel needs no orienting and a suspended magnetometer wants no gravity, so
// while the screen is off this part converts for nobody.
//
// A request, not the write, for the same reason the magnetometer's is
// (Compass.h): the screen state arrives on the display task and one board
// reads this part from the main loop, on the other core, over a bus whose
// other residents are read from tasks of their own. poll() below writes it.
// Safe from any task; idempotent; a no-op until begin() has found the part.
void setRunning(bool run);

// Whether the accelerometer is converting. False while suspended and false
// while absent; present() tells those apart.
bool running();

// Apply a pending setRunning(). Called from the main loop, which is the task
// that owns this part's bus access on the board that reads it for tilt, and
// is already on that bus on the board that reads it for panel rotation. Free
// when there is nothing to apply, which is every pass but the two either side
// of a screen going dark.
void poll();

} // namespace Imu

#else
namespace Imu {
enum class Facing : uint8_t { Up0, Up90, Up180, Up270, Flat, Unknown };
inline void begin() {}
inline bool present() { return false; }
inline Facing facing() { return Facing::Unknown; }
inline bool accel(float[3]) { return false; }
inline void setRunning(bool) {}
inline bool running() { return false; }
inline void poll() {}
} // namespace Imu
#endif // HAS_IMU
