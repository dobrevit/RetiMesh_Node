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
//  TouchInput.cpp — see TouchInput.h
//
//  The controller is a CHSC6X: a small capacitive part that holds one report
//  and answers I2C only while a finger is on the glass. The report is five
//  bytes — a point count, then two big-endian twelve-bit coordinates with
//  their high nibbles carrying flags to mask off. No registers to set up:
//  reset it, wait, and read when curious.
// ============================================================================
#include "TouchInput.h"

#if HAS_TOUCH

#include <Arduino.h>
#include <Wire.h>

namespace {

// The panel's second I2C controller: the main one may carry an OLED or a
// sensor on other boards, and this layer has its own pins on this one.
TwoWire sBus(1);
bool    sUp = false;

} // namespace

namespace TouchInput {

void begin() {
#if defined(PIN_TOUCH_RST) && PIN_TOUCH_RST >= 0
  // The reference driver's order: reset released first and given time, then a
  // deliberate reset pulse once the part has settled.
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(60);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(30);
#endif
  sUp = sBus.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
  if (!sUp) { log_w("touch: controller bus would not start (SDA %d, SCL %d)",
                    PIN_TOUCH_SDA, PIN_TOUCH_SCL); return; }
  // Who is actually on this bus. The controller is expected to NACK while
  // nothing touches the glass, so its own silence here proves little — but a
  // scan that finds a part at some *other* address, or a bus with nothing on
  // it at all, is exactly the wiring answer a dead touch layer needs.
  char found[48] = ""; size_t n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    sBus.beginTransmission(a);
    if (sBus.endTransmission() == 0 && n < sizeof(found) - 6)
      n += snprintf(found + n, sizeof(found) - n, " 0x%02x", a);
  }
  log_i("touch: bus up (SDA %d, SCL %d); acked now:%s (0x%02x expected; idle silence is normal)",
        PIN_TOUCH_SDA, PIN_TOUCH_SCL, n ? found : " nothing", TOUCH_ADDR);
}

Point poll() {
  Point p;
  if (!sUp) return p;
  // The controller is register-addressed: write the report pointer, then read
  // with a repeated start — a bare read gets nothing, which is why the first
  // version of this driver saw a dead layer under a working finger.
  sBus.beginTransmission((uint8_t)TOUCH_ADDR);
  sBus.write((uint8_t)0x00);
  if (sBus.endTransmission(false) != 0 ||
      sBus.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)8) != 8) return p;
  uint8_t r[8];
  for (uint8_t i = 0; i < 8; i++) r[i] = sBus.read();
  // The report's shape, from the reference driver: point count in byte 2,
  // first point in bytes 3-6, two poison patterns meaning "no report", and a
  // 0xC0 flag on byte 3 meaning the finger has lifted.
  const bool poison = (r[2] == 0 && r[3] == 0 && r[4] == 0 && r[6] == 0) ||
                      (r[2] == 0xFF && r[3] == 0xFF && r[4] == 0xFF && r[6] == 0xFF);
  if (poison || (r[2] & 0x07) == 0 || (r[3] & 0xC0) == 0xC0) return p;
  p.down = true;
  p.x = (int16_t)(((r[3] & 0x0F) << 8) | r[4]);
  p.y = (int16_t)(((r[5] & 0x0F) << 8) | r[6]);
  return p;
}

} // namespace TouchInput

#endif // HAS_TOUCH
