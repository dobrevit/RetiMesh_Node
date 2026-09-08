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
//  Environment.h — the air around the node
//
//  Temperature, pressure and humidity, from a BME280 where a board carries
//  one. The first sensor here whose readings are about the node's
//  surroundings rather than about the node: a gateway on a pole reports the
//  weather at the pole, and it does so whether or not anybody is looking at
//  its screen.
//
//  That last part is why this does not follow the screen the way the compass
//  and the accelerometer do (PeripheralPolicy.h). Their consumer is the lit
//  glass; this one's consumers are the console, the status API and — one day —
//  telemetry over the mesh, all of which are asked of a dark node. There is
//  nothing to suspend anyway: the part is left in sleep between readings and
//  woken for a single conversion, so its idle cost is a microamp and its busy
//  cost is about ten milliseconds once every ENV_SAMPLE_MS.
//
//  Read from one task, and it matters which
//  ----------------------------------------
//  poll() is called from the main loop and from nowhere else, which is not a
//  detail. I2cReg drains a read outside the bus lock (issue 33), so two tasks
//  *reading* one bus can each take some of the other's bytes — and on the
//  board this runs on, the panel is on the same bus. The panel only ever
//  writes, which the bus lock does cover, so one reader is the whole of the
//  safety here. Anything that wants a reading from another task asks last(),
//  which hands back the copy this loop already took.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_ENV

#include <stdint.h>

namespace Environment {

// One completed conversion. `valid` is false until the first one lands, and
// stays true afterwards with `atMs` saying how old it is — a stale reading is
// still the best answer available, and the caller is the one that knows how
// stale is too stale.
struct Reading {
  bool     valid       = false;
  float    tempC       = 0.0f;
  float    pressureHpa = 0.0f;
  float    humidityPct = 0.0f;
  uint32_t atMs        = 0;
};

// Find the part and read its calibration. Safe to call with nothing fitted.
void begin();

// Whether a part answered its chip id and gave up a calibration block.
bool present();

// The last completed conversion. Safe from any task.
Reading last();

// How long ago that reading was taken, in seconds. Here rather than at each
// surface because three of them print it — the console, the status API and the
// glass — and "millis() minus atMs, divided by a thousand" written three times
// is the one rule this sensor was repeating per surface.
uint32_t ageS(const Reading& r);

// How many sampling intervals have passed with no reading, once one has been
// asked for. Zero while the sensor is answering. A part that is fitted and
// never becomes ready would otherwise leave every surface saying "waiting for
// the first reading" for hours, which is a promise rather than a report.
uint32_t missedIntervals();

// Trigger a conversion, or collect one that has had time to finish. Called
// from the main loop; free on every pass but the two that do the work, and it
// never blocks waiting on the part.
void poll();

} // namespace Environment

#else

namespace Environment {
struct Reading {
  bool     valid       = false;
  float    tempC       = 0.0f;
  float    pressureHpa = 0.0f;
  float    humidityPct = 0.0f;
  uint32_t atMs        = 0;
};
inline void begin() {}
inline bool present() { return false; }
inline Reading last() { return Reading{}; }
inline uint32_t ageS(const Reading&) { return 0; }
inline uint32_t missedIntervals() { return 0; }
inline void poll() {}
} // namespace Environment

#endif // HAS_ENV
