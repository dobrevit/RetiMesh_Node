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
//  Power.cpp — see Power.h
// ============================================================================
#include "Power.h"
#include <WiFi.h>
#include <esp32-hal-cpu.h>
#include "Settings.h"

namespace {
Power::Profile sProfile = Power::Profile::Performance;
float    sVolts = 0;
uint32_t sLastSample = 0;

// Single-cell LiPo open-circuit voltage to a rough percentage.
uint8_t percentFor(float v) {
  static const float pts[][2] = { {4.20, 100}, {4.06, 90}, {3.98, 80}, {3.92, 70}, {3.87, 60},
                                  {3.82, 50}, {3.79, 40}, {3.77, 30}, {3.74, 20}, {3.68, 10}, {3.45, 0} };
  if (v >= pts[0][0]) return 100;
  for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) {
    if (v >= pts[i][0]) {
      float f = (v - pts[i][0]) / (pts[i-1][0] - pts[i][0]);
      return (uint8_t)(pts[i][1] + f * (pts[i-1][1] - pts[i][1]));
    }
  }
  return 0;
}

void sample() {
  // 12-bit ADC with 11 dB attenuation reads up to ~3.1 V; the divider halves
  // the cell voltage. Average a few readings; the pin floats without a cell.
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) acc += analogReadMilliVolts(PIN_BATTERY_ADC);
  sVolts = (acc / 8) / 1000.0f * BATTERY_DIVIDER_RATIO;
  sLastSample = millis();
}
}

namespace Power {

const char* profileName(Profile p) {
  switch (p) { case Profile::Balanced: return "balanced"; case Profile::Battery: return "battery"; default: return "performance"; }
}

bool profileFromName(const char* n, Profile& out) {
  if (!n) return false;
  if (!strcmp(n, "performance")) { out = Profile::Performance; return true; }
  if (!strcmp(n, "balanced"))    { out = Profile::Balanced;    return true; }
  if (!strcmp(n, "battery"))     { out = Profile::Battery;     return true; }
  return false;
}

Profile profile() { return sProfile; }

void apply(Profile p) {
  sProfile = p;
  switch (p) {
    case Profile::Battery:     setCpuFrequencyMhz(80);  WiFi.setSleep(true);  break;
    case Profile::Balanced:    setCpuFrequencyMhz(160); WiFi.setSleep(true);  break;
    default:                   setCpuFrequencyMhz(240); WiFi.setSleep(false); break;
  }
  log_i("power profile: %s (CPU %u MHz, Wi-Fi sleep %s)", profileName(p), (unsigned)getCpuFrequencyMhz(),
        p == Profile::Performance ? "off" : "on");
}

uint32_t displaySleepMs() {
  return sProfile == Profile::Battery ? DISPLAY_SLEEP_BATTERY_MS : DISPLAY_SLEEP_MS;
}

void begin() {
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  sample();
  apply((Profile)settings.transport().powerProfile);
}

Battery battery() {
  if (millis() - sLastSample > BATTERY_SAMPLE_MS) sample();
  Battery b;
  b.volts   = sVolts;
  b.present = sVolts >= BATTERY_MIN_V && sVolts <= BATTERY_MAX_V;
  b.percent = b.present ? percentFor(sVolts) : 0;
  return b;
}

} // namespace Power
