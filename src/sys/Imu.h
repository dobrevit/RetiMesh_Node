// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Imu.h — which way up the node is, from the DA217 in the case
//
//  One question is asked of six axes: which edge of the panel points down,
//  so the display can follow the hand holding it. The reading is raw and
//  unfiltered here; the caller debounces, because only the caller knows what
//  a twitch costs on its glass.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_DA217

#include <stdint.h>

namespace Imu {

enum class Facing : uint8_t { Up0, Up90, Up180, Up270, Flat, Unknown };

void begin();                            // probe and start the accelerometer
bool present();

Facing facing();                         // one fresh reading's verdict

} // namespace Imu

#else
namespace Imu {
enum class Facing : uint8_t { Up0, Up90, Up180, Up270, Flat, Unknown };
inline void begin() {}
inline bool present() { return false; }
inline Facing facing() { return Facing::Unknown; }
} // namespace Imu
#endif // HAS_DA217
