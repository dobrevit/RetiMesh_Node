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
//  Power.h — power profiles and battery gauge
//
//  Profiles (settings, default "performance" = today's behaviour):
//    performance  240 MHz, Wi-Fi always on, display sleeps after 60 s
//    balanced     160 MHz, Wi-Fi modem sleep on the station link
//    battery       80 MHz, Wi-Fi modem sleep, display sleeps after 20 s
//  LoRa and Transport timing are unaffected by the CPU clock at these
//  rates; the radio task waits on interrupts either way.
//
//  Battery: the T3-S3 exposes the cell through a 100k/100k divider on
//  GPIO 1. Voltage below BATTERY_MIN_V means "no battery" (USB-only bench).
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace Power {

enum class Profile : uint8_t { Performance = 0, Balanced = 1, Battery = 2 };

void begin();                       // applies the configured profile, starts sampling
void apply(Profile p);              // live switch
Profile profile();
const char* profileName(Profile p);
bool profileFromName(const char* name, Profile& out);

struct Battery {
  bool  present;                    // voltage in a plausible cell range
  float volts;
  uint8_t percent;                  // rough LiPo curve; 0 when absent
};
Battery battery();
uint32_t displaySleepMs();          // profile-dependent

} // namespace Power
