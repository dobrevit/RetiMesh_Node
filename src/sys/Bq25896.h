// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Bq25896.h — the charge manager on boards whose case carries one
//
//  The V4's TFT assembly puts a BQ25896 between the cell and everything else,
//  and its /QON pin is what the case's power button presses: power-on from
//  cold is the charger's hardware, not ours. What the firmware gets over I2C
//  is the other half — an honest answer to "is the cell charging" that a bare
//  divider can never give, and ship mode: BATFET off, a hard power-off that
//  only the button (or a charger plug-in) undoes.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_BQ25896

namespace Bq25896 {

// Probe the part on the main I2C bus. Safe to call once at boot; everything
// below answers no/false until it has succeeded.
void begin();
bool present();

bool charging();                         // CHRG_STAT: pre or fast charge
bool vbusPowered();                      // a charger or USB is feeding VBUS

// BATFET off: the node is dead until the power button (QON) or a charger
// brings it back. Does not return when it works.
void shipMode();

} // namespace Bq25896

#else
namespace Bq25896 {
inline void begin() {}
inline bool present() { return false; }
inline bool charging() { return false; }
inline bool vbusPowered() { return false; }
inline void shipMode() {}
} // namespace Bq25896
#endif // HAS_BQ25896
