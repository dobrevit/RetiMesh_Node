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

#include "Leds.h"
#include <Arduino.h>
#include "Config.h"
#include "Bootloader.h"
#include "LocalLink.h"
#include "Power.h"

#define HAS_LEDS (PIN_STATUS_LED >= 0 || PIN_WIFI_LED >= 0 || PIN_LORA_LED >= 0)

namespace Leds {

#if HAS_LEDS
// A flicker is the LED inverted for this long after a counter moved: long
// enough to be seen at the loop's cadence, short enough to read as activity
// rather than as the service going up and down.
static constexpr uint32_t kFlickerMs = 150;

static void set(int pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, (on == (bool)LED_ACTIVE_HIGH) ? HIGH : LOW);
}

// Whether a counter moved since the last pass, and a flicker window if so.
struct Activity {
  uint32_t seen = 0, until = 0;
  bool flicker(uint32_t count, uint32_t nowMs) {
    if (count != seen) { seen = count; until = nowMs + kFlickerMs; }
    return (int32_t)(until - nowMs) > 0;
  }
};
static Activity sWifi, sLora;

static bool wifiReady() {
  const LocalLink::Link* ap  = LocalLink::find(LocalLink::Type::WifiAp);
  const LocalLink::Link* sta = LocalLink::find(LocalLink::Type::WifiSta);
  return (ap && ap->address()) || (sta && sta->address());
}

void begin() {
  for (int pin : { (int)PIN_STATUS_LED, (int)PIN_WIFI_LED, (int)PIN_LORA_LED }) {
    if (pin < 0) continue;
    pinMode(pin, OUTPUT);
    set(pin, false);
  }
}

void tick(uint32_t nowMs) {
  if (Power::profile() == Power::Profile::Battery) {
    set(PIN_STATUS_LED, false); set(PIN_WIFI_LED, false); set(PIN_LORA_LED, false);
    return;
  }
  // Status: up when the transport is; a restart on its way blinks it, so a
  // node that is about to go away says so before it does.
  bool status = g_stats.transportOnline;
  if (Bootloader::pending()) status = (nowMs / 250) & 1;
  // Wi-Fi: a link ready; traffic from TCP clients flickers it.
  bool wifi = wifiReady();
  if (sWifi.flicker(g_stats.tcpRxPackets, nowMs)) wifi = !wifi;
  // LoRa: the radio online; every packet either way flickers it.
  bool lora = g_stats.radioOnline;
  if (sLora.flicker(g_stats.loraRxPackets + g_stats.loraTxPackets, nowMs)) lora = !lora;
  set(PIN_STATUS_LED, status);
  set(PIN_WIFI_LED, wifi);
  set(PIN_LORA_LED, lora);
}
#else
void begin() {}
void tick(uint32_t) {}
#endif

} // namespace Leds
