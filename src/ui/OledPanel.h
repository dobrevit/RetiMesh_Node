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
//  OledPanel.h — the monochrome I2C panel behind the Panel interface
//
//  Everything here was in Display::begin(): the switched rail, the stuck-bus
//  recovery, the two-address probe and the driver's own start-up. It is panel
//  work rather than page work, and it moved so that the next panel has a place
//  to put its equivalent instead of adding a branch to the display module.
//
//  Two controllers, one panel driver
//  ---------------------------------
//  Boards here carry an SSD1306 or, on the T-Beam Supreme, an SH1106. From the
//  page's side they are the same panel: 128x64 monochrome on I2C, drawn with
//  the same GFX calls, one full-frame update costing a kilobyte on the bus.
//  What differs is small and unforgiving — the SH1106 has 132 columns of RAM
//  with the glass wired to the middle 128, and no horizontal addressing mode,
//  so the SSD1306's driver writes it two columns out of place and wraps what
//  falls off the end. And nothing can be probed for: an acknowledgement at
//  the board's configured address says a part is there and nothing about
//  which controller it is, so the board declares it (OLED_CONTROLLER in
//  Config.h) — as it declares the address, which is not the same on every
//  board (OLED_ADDR).
//
//  Which is why this is one class with a driver type rather than two panels.
//  Every difference between the parts is in the four lines below — the type,
//  the ink, the two commands and the start-up call — and none of it is page
//  work, so a second Panel implementation would have been the same file twice
//  for the sake of a two-column offset that its own library already handles.
// ============================================================================
#pragma once

#include "Panel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_OLED

#include <Wire.h>
#if OLED_CONTROLLER == OLED_CONTROLLER_SH1106
  #include <Adafruit_SH110X.h>
#else
  #include <Adafruit_SSD1306.h>
#endif

class OledPanel : public Panel {
public:
#if OLED_CONTROLLER == OLED_CONTROLLER_SH1106
  using Driver = Adafruit_SH1106G;
  static constexpr const char* kController = "SH1106";
  static constexpr uint16_t kInk   = SH110X_WHITE;
  static constexpr uint16_t kPaper = SH110X_BLACK;
  static constexpr uint8_t  kOff   = SH110X_DISPLAYOFF;
  static constexpr uint8_t  kOn    = SH110X_DISPLAYON;
  static constexpr uint8_t  kSetContrast = SH110X_SETCONTRAST;
#else
  using Driver = Adafruit_SSD1306;
  static constexpr const char* kController = "SSD1306";
  static constexpr uint16_t kInk   = SSD1306_WHITE;
  static constexpr uint16_t kPaper = SSD1306_BLACK;
  static constexpr uint8_t  kOff   = SSD1306_DISPLAYOFF;
  static constexpr uint8_t  kOn    = SSD1306_DISPLAYON;
  static constexpr uint8_t  kSetContrast = SSD1306_SETCONTRAST;
#endif

  bool begin() override;
  bool present() const override { return _ok; }
  Adafruit_GFX& gfx() override { return _oled; }
  void clear() override { _oled.clearDisplay(); }
  // These panels have one kind of update and it costs a kilobyte over I2C, so
  // a full refresh is the same call: there is no ghosting to clear.
  void flush(bool) override { _oled.display(); }
  void blank(bool on) override { command(on ? kOff : kOn); }
  // Panel current is close to linear in contrast on both parts, so this is a
  // real power knob rather than a cosmetic one. The percent-to-level mapping
  // is DisplayLayout's, shared with the TFT backlight rather than written out
  // again here. Both controllers take the level as the byte after the same
  // 0x81 command.
  void setBrightness(uint8_t pct) {
    command(kSetContrast);
    command(DisplayLayout::brightnessLevel(pct));
  }
  // The panel and its charge pump come off, which is the point of the timer.
  bool blanks() const override { return true; }
  const uint8_t* frame(size_t& len) const override {
    len = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT / 8;
    return const_cast<Driver&>(_oled).getBuffer();
  }
  uint16_t ink() const override { return kInk; }
  uint16_t paper() const override { return kPaper; }

  // Which of the two addresses answered, for the log and for /api/status.
  uint8_t address() const { return _addr; }

private:
  static bool ack(uint8_t addr);
  // The one call the two libraries spell differently. Adafruit's SSD1306 has
  // its own; the SH110X inherits GrayOLED's.
  void command(uint8_t c) {
#if OLED_CONTROLLER == OLED_CONTROLLER_SH1106
    _oled.oled_command(c);
#else
    _oled.ssd1306_command(c);
#endif
  }
  Driver  _oled{DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, PIN_OLED_RST};
  bool    _ok   = false;
  uint8_t _addr = 0;
};

#endif // HAS_DISPLAY && OLED
