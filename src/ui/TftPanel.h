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
//  TftPanel.h — the 240x320 colour panel behind the Panel interface
//
//  Driven directly: an ST7789 wants a reset pulse, eight init commands and a
//  window to stream pixels into, which is a page of code — less than the
//  workarounds for any driver library's dependency tree, and the same shape as
//  the e-paper panel beside it: pages draw on a canvas this class owns, and
//  flush() puts the canvas on the glass.
//
//  The canvas is half the panel's resolution, deliberately. At this panel's
//  dot pitch the 6x8 font the pages are written in is under a millimetre tall;
//  drawn at half resolution and shown as 2x2 blocks it lands at the size the
//  e-paper shows it, and every page works unchanged. The doubling is done a
//  line at a time on the way out, so nothing but this file knows the glass is
//  finer than the drawing (DisplayLayout.h explains the halving; the board
//  header keeps the panel's true size).
//
//  Monochrome for now — the canvas is one bit deep and flush() maps ink to
//  white on black, which is what every page already draws. The panel could
//  show colour; the day a page wants it, the canvas is the thing to widen,
//  and this interface does not change.
//
//  Unlike the OLED and the e-paper there is no way to ask whether the panel
//  is there: it is write-only — no MISO, no BUSY — so begin() succeeds when
//  the canvas allocates and the wiring is taken on the board header's word.
//  A missing panel costs the SPI writes and harms nothing.
// ============================================================================
#pragma once

#include "Panel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_TFT

#include <SPI.h>

class TftPanel : public Panel {
public:
  bool begin() override;
  bool present() const override { return _ok; }
  // The canvas, not the panel: pages get real Adafruit_GFX at the drawing
  // resolution and never learn the glass is finer.
  Adafruit_GFX& gfx() override { return *_canvas; }
  void clear() override { if (_canvas) _canvas->fillScreen(0); }
  // Streams the band of rows that changed since the last flush — a one-line
  // change costs a few panel lines, not the whole ~150 KB frame. `full`
  // forces the lot; there is nothing to ghost, so nothing else needs it.
  void flush(bool full) override;
  // A backlight is most of what this panel costs, so blanking is real money:
  // the glass goes dark and the LED goes off.
  void blank(bool on) override;
  bool blanks() const override { return true; }
  const uint8_t* frame(size_t& len) const override {
    len = (size_t)((kW + 7) / 8) * kH;
    return _canvas ? _canvas->getBuffer() : nullptr;
  }
  // On the canvas a set bit is ink; flush() decides what that is on the glass.
  uint16_t ink() const override { return 1; }
  uint16_t paper() const override { return 0; }

  // The GUI path: a rectangle of big-endian RGB565 pixels straight to the
  // glass at full resolution. The mono canvas, its doubling and its shadow
  // play no part — LVGL keeps its own idea of what changed and hands over
  // exactly that. Coordinates are panel-native and inclusive.
  void blitArea(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const uint8_t* px);

private:
  // Drawing geometry, from the layout — the one declaration of it
  // (DisplayLayout.h's kTft120x160); the board header's DISPLAY_WIDTH and
  // DISPLAY_HEIGHT are the physical panel, which must be exactly twice this.
  static constexpr int16_t kW = DisplayLayout::active().width;
  static constexpr int16_t kH = DisplayLayout::active().height;
  static_assert(kW * 2 == DISPLAY_WIDTH && kH * 2 == DISPLAY_HEIGHT,
                "the canvas doubles onto the glass; the two must be exactly 2:1");

  void cmd(uint8_t c);
  void cmd(uint8_t c, const uint8_t* data, size_t len);
  void window(int16_t x0, int16_t y0, int16_t x1, int16_t y1);  // panel rect, ready for pixels

  SPIClass    _spi{TFT_SPI_BUS};
  GFXcanvas1* _canvas = nullptr;
  // What the glass last saw, so flush() streams only the band of rows that
  // changed: a ticking counter costs one band, not the whole 153 KB frame.
  uint8_t*    _shadow = nullptr;
  bool        _ok = false;
  bool        _lit = false;             // backlight state, so blank() is idempotent
};

#endif // HAS_DISPLAY && TFT
