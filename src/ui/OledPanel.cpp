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
//
//  One file for both controllers: the rail, the stuck-bus recovery and the
//  address probe are the same work on either part, and only the driver's own
//  start-up call differs.
// ============================================================================
#include "OledPanel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_OLED

#include "esp32-hal-periman.h"
#include "DisplayLayout.h"

// Addressed and nothing more. A zero-length write asks "is anybody there" and
// leaves no mark, which matters on a bus where the answer might not be a panel:
// the T-Beam Supreme has two addresses that answer this, 0x3c and 0x3d, and only
// one of them is the glass.
//
// Sending a command instead — the 0x00 control byte and a NOP — was tried, on
// the theory that a panel would take it and anything else would refuse. It does
// not discriminate: that pair is a perfectly legal "write 0xe3 to register 0"
// for any register chip, so both addresses took it. It also writes a byte into
// whichever part answers first, which is a side effect on a device this code
// has no business touching. So the probe stays a question, and the board says
// which address is the panel (OLED_ADDR).
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
  panelVextOn();

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
    const bool took = ack(a);
    // Both answers are logged, not just the winning one: on a board where two
    // addresses answer, which of them took a command is the fact that explains
    // a panel that stays dark or keeps somebody else's picture.
    log_i("display: 0x%02X %s", a, took ? "took a command" : "did not answer");
    if (took && _addr == 0) _addr = a;
  }
  if (_addr == 0) {
    log_w("No I2C device at 0x%02X/0x%02X (SDA %d / SCL %d) — display disabled",
          candidates[0], candidates[1], PIN_OLED_SDA, PIN_OLED_SCL);
    return false;
  }

#if OLED_CONTROLLER == OLED_CONTROLLER_SH1106
  // No periphBegin flag on this driver: it calls begin() on the bus itself,
  // which the core turns into a warning and a no-op on a host that is already
  // started ("Bus already started in Master Mode", Wire.cpp) — so the pins set
  // above are the ones that stand. The reset argument is the panel's RST line
  // and does nothing on a board that ties it high.
  if (!_oled.begin(_addr, true)) {
#else
  // periphBegin=false: Wire is already up on the board-specific pins.
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, _addr, true, false)) {
#endif
    log_w("%s at 0x%02X did not initialise — display disabled", kController, _addr);
    return false;
  }
  log_i("%s found at 0x%02X (SDA %d / SCL %d)", kController, _addr,
        PIN_OLED_SDA, PIN_OLED_SCL);
  _oled.setRotation(OLED_ROTATION);
  _oled.clearDisplay();
  _oled.setTextColor(kInk);
  _oled.setTextSize(1);
  // Adafruit_GFX wraps by default, so a row one character too long lands on
  // the next row and, at the bottom, under the page dots. Clip instead.
  _oled.setTextWrap(false);
  _ok = true;
  return _ok;
}

#endif // HAS_DISPLAY && OLED
