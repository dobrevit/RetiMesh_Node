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
//  Leds.h — what the board's LEDs say
//
//  A node in a box has no display and no one watching its log; its LEDs are
//  the whole of what it can show. Dark is the normal state: the service is up
//  and nothing is passing. A flicker is traffic. A lamp that stays lit is the
//  one thing worth a glance — that service is meant to be up and is not:
//
//    status  lit while the transport is down; blinks while a restart is on
//            its way
//    wifi    lit while Wi-Fi is switched on but no link is ready; flickers on
//            traffic from TCP clients
//    lora    lit while the radio is offline; flickers on every packet sent
//            or received
//
//  So at boot every LED lights and goes out as its service comes up, which
//  is the boot indication a headless board needs, and a box that shows a
//  steady lamp in the evening has something to say.
//
//  A board names the LEDs it has (PIN_STATUS_LED, PIN_WIFI_LED, PIN_LORA_LED
//  in its board header; -1 for none) and whether they light on HIGH
//  (LED_ACTIVE_HIGH). An LED the board labels for something the firmware
//  does not do — the Wireless Bridge's BLE lamp — is named as PIN_BLE_LED
//  and held dark, because a lit lamp with that label would be a claim. In
//  the Battery power profile every LED stays dark: a relay on a cell has
//  better uses for the current.
//
//  Driven from loop(), on its cadence; nothing here blocks or sleeps.
// ============================================================================
#pragma once

#include <stdint.h>

namespace Leds {

// Claims the pins the board names and turns every LED off.
void begin();

// One pass: reads the services' state and the traffic counters, sets the LEDs.
void tick(uint32_t nowMs);

} // namespace Leds
