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
//  Pmu.h — the power-management chip on boards that have one
//
//  On a T-Beam the transceiver, the GPS and the OLED are not wired straight
//  to 3V3: each hangs off a regulator inside an AXP192 (v1.1) or AXP2101
//  (v1.2), and comes up *off*. Probing the radio before switching its rail on
//  finds nothing at all — so begin() runs first in setup(), and everything
//  else can then behave as if the peripherals were simply present.
//
//  The two chips answer at the same I2C address and are told apart by their
//  chip id, which XPowersLib does for us. The GPS rail is left off: nothing
//  reads it yet and it costs tens of milliamps.
//
//  The same chip measures the cell, so on these boards Power.cpp asks here
//  instead of reading an ADC divider.
// ============================================================================
#pragma once

#include "Config.h"

namespace Pmu {

struct Battery {
  bool  present  = false;
  bool  charging = false;
  float volts    = 0.0f;
  uint8_t percent = 0;
};

// Detects the chip and powers the rails the node needs. False when no PMU
// answered — on a board with HAS_PMU that means the radio will not respond
// either, which is worth saying out loud in the log.
bool begin();

bool present();
const char* model();             // "AXP192", "AXP2101" or "none"

Battery battery();

// The GPS rail, off until something wants it.
void gpsPower(bool on);
bool gpsPowered();

} // namespace Pmu
