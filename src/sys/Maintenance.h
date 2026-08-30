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
//  Maintenance.h — the console on the serial port
//
//  The port a host has anyway — the S3's USB CDC, the Heltec's CP2102 —
//  carried only the log. Now it answers: VERSION, STATUS, the state of every
//  local link, and the two commands a flashing tool needs, RESET CONFIRM and
//  BOOTLOADER CONFIRM. The wire format is in MaintenanceProtocol.h and is the
//  same one the CDC-ACM function of the composite USB device will speak,
//  so a host script written against this port keeps working there.
//
//  Log lines share the port. Replies all begin with "RM ", log lines never
//  do, and a request line is never echoed, so a host filters on the prefix.
//  Runs on the loop task, a few bytes per pass; it never blocks and never
//  allocates.
// ============================================================================
#pragma once

#include <Arduino.h>

namespace Maintenance {

void begin(Stream& io);        // announces itself with "RM HELLO ..."
void poll();                   // from loop(); reads what has arrived, answers complete lines

} // namespace Maintenance
