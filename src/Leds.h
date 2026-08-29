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
//  the whole of what it can show. Each one stands for a service and says two
//  things: whether the service is up (steady), and whether it is doing
//  anything (a flicker as traffic passes):
//
//    status  the transport is up; blinks while a restart is on its way
//    wifi    the access point or the station is ready; flickers on traffic
//            from TCP clients
//    lora    the radio is online; flickers on every packet sent or received
//
//  A board names the LEDs it has (PIN_STATUS_LED, PIN_WIFI_LED, PIN_LORA_LED
//  in its board header; -1 for none) and whether they light on HIGH
//  (LED_ACTIVE_HIGH). In the Battery power profile every LED stays dark: a
//  relay on a cell has better uses for the current.
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
