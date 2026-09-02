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
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(50);                             // the part wants a moment after reset
#endif
  sUp = sBus.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
  if (!sUp) log_w("touch: controller bus would not start (SDA %d, SCL %d)",
                  PIN_TOUCH_SDA, PIN_TOUCH_SCL);
}

Point poll() {
  Point p;
  if (!sUp) return p;
  // The controller simply does not ACK while nothing touches the glass, so a
  // failed request is the idle case and costs a couple of I2C clocks.
  if (sBus.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)5) != 5) return p;
  uint8_t r[5];
  for (uint8_t i = 0; i < 5; i++) r[i] = sBus.read();
  if (r[0] == 0) return p;               // answered, but no contact
  p.down = true;
  p.x = (int16_t)(((r[1] & 0x0F) << 8) | r[2]);
  p.y = (int16_t)(((r[3] & 0x0F) << 8) | r[4]);
  return p;
}

} // namespace TouchInput

#endif // HAS_TOUCH
