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
//  OledPanel.h — the SSD1306 behind the Panel interface
//
//  Everything here was in Display::begin(): the switched rail, the stuck-bus
//  recovery, the two-address probe and the driver's own start-up. It is panel
//  work rather than page work, and it moved so that the next panel has a place
//  to put its equivalent instead of adding a branch to the display module.
// ============================================================================
#pragma once

#include "Panel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_OLED

#include <Wire.h>
#include <Adafruit_SSD1306.h>

class OledPanel : public Panel {
public:
  bool begin() override;
  bool present() const override { return _ok; }
  Adafruit_GFX& gfx() override { return _oled; }
  void clear() override { _oled.clearDisplay(); }
  // An SSD1306 has one kind of update and it costs a kilobyte over I2C, so a
  // full refresh is the same call: there is no ghosting to clear.
  void flush(bool) override { _oled.display(); }
  void blank(bool on) override {
    _oled.ssd1306_command(on ? SSD1306_DISPLAYOFF : SSD1306_DISPLAYON);
  }
  const uint8_t* frame(size_t& len) const override {
    len = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT / 8;
    return const_cast<Adafruit_SSD1306&>(_oled).getBuffer();
  }
  uint16_t ink() const override { return SSD1306_WHITE; }
  uint16_t paper() const override { return SSD1306_BLACK; }

  // Which of the two addresses answered, for the log and for /api/status.
  uint8_t address() const { return _addr; }

private:
  static bool ack(uint8_t addr);
  Adafruit_SSD1306 _oled{DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, PIN_OLED_RST};
  bool    _ok   = false;
  uint8_t _addr = 0;
};

#endif // HAS_DISPLAY && OLED
