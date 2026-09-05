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
//  Battery: boards with a power-management chip (HAS_PMU) ask it — it knows
//  whether a cell is connected and whether it is charging. Otherwise the
//  T3-S3 exposes the cell through a 100k/100k divider on
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
  bool  present;                    // a cell is connected
  bool  charging = false;           // meaningless unless chargeKnown
  // Whether charging is a question this board can answer at all. A divider
  // measures the cell and nothing else: the charger on an ADC-only board does
  // its work in hardware and tells the processor nothing, and there is no
  // status line to read. Reporting "not charging" there is a claim the board
  // cannot support — a plugged-in node insisting it is not charging is worse
  // than one admitting it has no idea, because the first sends someone looking
  // for a fault in a cable that is working.
  bool  chargeKnown = false;
  float volts;
  uint8_t percent;                  // rough LiPo curve; 0 when absent
};
Battery battery();

// Whether the last reading is too old to believe. battery() reports present =
// false in that case, which is the honest answer and the one every caller
// already renders — but it is the same answer as "no cell is fitted", and
// during a bring-up those want telling apart. False on a board with no divider
// to go stale.
bool readingStale();
uint32_t displaySleepMs();          // profile-dependent

// The last eight hours of charge, one percent-point per five minutes,
// oldest first — the discharge sparkline's data. Returns how many exist.
size_t batteryHistory(uint8_t* out, size_t max);

} // namespace Power
