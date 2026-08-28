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
//  Bootloader.h — every restart the node makes goes through here
//
//  A restart used to be a deadline in WifiManager and an ESP.restart() in
//  loop(). Now there is one place that knows how to stop the node: it takes
//  requests from the settings API, the maintenance console, the bootloader
//  API and (later) the USB touch, lets the acknowledgement leave, refuses
//  new work, flushes what it can, and restarts — into the application, or
//  into the ROM downloader on silicon that can be asked to.
//
//  Callers never see the mechanism. A T3-S3 and a Heltec V3 both answer
//  "software_api" to canEnterAutomatically(); a T-Beam answers no, and its
//  plan says the bridge does it instead. The rules are in BootloaderPlan.h
//  (pure, tested); this is what carries them out on the chip.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "BootloaderPlan.h"

namespace Bootloader {

// What this board can do, worked out from the silicon and the board flags.
Caps caps();
Plan plan();
bool canEnterAutomatically();

// Ask for a restart `delayMs` from now. Returns false and sets *whyNot when
// it cannot be honoured: a bootloader request on silicon without the bit, or
// a request that arrives while one is already going through.
bool request(Target target, Source source, uint32_t delayMs, const char** whyNot = nullptr);

// Convenience for the paths that already existed: settings saves and moves.
inline bool reboot(uint32_t delayMs, Source source = Source::Settings) {
  return request(Target::App, source, delayMs);
}

// Once a request is in, services should refuse new work.
bool   pending();
State  state();
Target target();

// From loop(): runs the sequence. Never returns from the Restart step.
void tick();

// The recovery text, for every reply that mentions the bootloader.
const char* manualRecovery();

} // namespace Bootloader
