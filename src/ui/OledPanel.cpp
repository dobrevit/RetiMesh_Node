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
//  OledPanel.cpp — see OledPanel.h
// ============================================================================
#include "OledPanel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_OLED

#include "esp32-hal-periman.h"
#include "DisplayLayout.h"

bool OledPanel::ack(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;    // 0 = ACK received
}

// A reset that lands in the middle of a transfer — esptool's after a flash,
// the RST button, a watchdog — leaves the panel holding SDA low, waiting for
// clocks that never come, and it holds it until it loses power. The probe in
// begin() then finds nothing, the display task never starts, and the board
// runs dark until somebody pulls the plug. So the bus is clocked free before
// anything else touches it: up to nine pulses on SCL with SDA released, then
// a STOP, all of which is a no-op on a bus that is idle.
static void releaseBus(int sda, int scl) {
  // Only where the display is first on its bus. The T-Beam's PMU has opened
  // Wire on these pins before the display looks, and a pinMode() on a pin
  // the core's peripheral manager has given to a driver tears that driver
  // down, for Wire.begin() below to build again. There the bus is left as
  // the PMU found it.
  if (perimanGetPinBus(sda, ESP32_BUS_TYPE_I2C_MASTER_SDA) != nullptr) return;
  // Open-drain with the internal pull-ups, the same way Wire drives the
  // lines: a board with no external resistors gets its clocks that way too.
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, OUTPUT_OPEN_DRAIN | PULLUP);
  digitalWrite(scl, HIGH);
  delayMicroseconds(5);
  int pulses = 0;
  while (digitalRead(sda) == LOW && pulses < 9) {
    digitalWrite(scl, LOW);  delayMicroseconds(5);
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    pulses++;
  }
  if (pulses) {
    // STOP: SDA rising while SCL is high.
    pinMode(sda, OUTPUT_OPEN_DRAIN | PULLUP);
    digitalWrite(sda, LOW);  delayMicroseconds(5);
    digitalWrite(scl, HIGH); delayMicroseconds(5);
    digitalWrite(sda, HIGH); delayMicroseconds(5);
    log_w("display: the panel was holding SDA low from an interrupted transfer; released after %d clocks", pulses);
  }
}

bool OledPanel::begin() {
#if HAS_DISPLAY_VEXT
  // The panel is fed from a switched rail, active low, and needs a moment to
  // settle before it will answer. Without this the I2C probe finds nothing and
  // the board looks broken rather than switched off.
  pinMode(PIN_DISPLAY_VEXT, OUTPUT);
  digitalWrite(PIN_DISPLAY_VEXT, LOW);
  delay(50);
#endif

  releaseBus(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setTimeOut(50);                   // a missing panel must not stall boot

  #if PIN_OLED_RST >= 0
    // Release the panel from reset before probing for it. The driver does this
    // too, but only inside begin() — which runs after the probe below, so a
    // panel still held in reset never answers and is written off as absent.
    // Boards that tie reset high have PIN_OLED_RST at -1 and skip this.
    pinMode(PIN_OLED_RST, OUTPUT);
    digitalWrite(PIN_OLED_RST, LOW);
    delay(20);
    digitalWrite(PIN_OLED_RST, HIGH);
    delay(20);
  #endif

  const uint8_t candidates[] = { OLED_ADDR, (uint8_t)(OLED_ADDR == 0x3C ? 0x3D : 0x3C) };
  for (uint8_t a : candidates) {
    if (ack(a)) { _addr = a; break; }
  }
  if (_addr == 0) {
    log_w("No I2C device at 0x%02X/0x%02X (SDA %d / SCL %d) — display disabled",
          candidates[0], candidates[1], PIN_OLED_SDA, PIN_OLED_SCL);
    return false;
  }

  // periphBegin=false: Wire is already up on the board-specific pins.
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, _addr, true, false)) {
    log_w("SSD1306 at 0x%02X did not initialise — display disabled", _addr);
    return false;
  }
  log_i("SSD1306 found at 0x%02X (SDA %d / SCL %d)", _addr, PIN_OLED_SDA, PIN_OLED_SCL);
  _oled.setRotation(OLED_ROTATION);
  _oled.clearDisplay();
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setTextSize(1);
  // Adafruit_GFX wraps by default, so a row one character too long lands on
  // the next row and, at the bottom, under the page dots. Clip instead.
  _oled.setTextWrap(false);
  _ok = true;
  return _ok;
}

#endif // HAS_DISPLAY && OLED
