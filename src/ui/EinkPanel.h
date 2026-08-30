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
//  EinkPanel.h — the 2.13" e-paper on the Heltec Wireless Paper
//
//  The panel this firmware's display abstraction was written for. Two things
//  about it are unlike the OLED, and both are why the abstraction exists:
//
//  An update costs. A full refresh takes the better part of a second and
//  flashes the panel black-white-black while it runs; a partial one is faster
//  and quiet but leaves a faint record of what was there before. So the page
//  is drawn into memory on every pass and RefreshPolicy decides what reaches
//  the glass — nothing when the frame is unchanged, a partial update when it
//  is, and a full one every few partials to clear the ghosting they leave.
//
//  It draws with the wrong GFX. The driver library derives its display from
//  its own fork of Adafruit_GFX rather than from Adafruit_GFX itself, so it
//  cannot be handed to pages that expect the real one. Rather than rewrite
//  every page against a second GFX, the pages draw on an Adafruit_GFX canvas
//  this class owns, and flush() blits that canvas into the driver. The canvas
//  earns its 3904 bytes twice over: it is also the frame the refresh policy
//  compares, which the driver's own buffer would not give us.
// ============================================================================
#pragma once

#include "Panel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_EINK

#include <heltec-eink-modules.h>

// The board header and the driver library each name these pins, and the
// driver is the one that acts on them: it takes its own PIN_DISPLAY_* rather
// than anything passed in. So the two have to agree, and a build where they
// do not is a boot log confidently printing pins the panel is not on.
static_assert(PIN_EPD_DC   == PIN_DISPLAY_DC,   "board header and driver disagree about the panel's DC pin");
static_assert(PIN_EPD_CS   == PIN_DISPLAY_CS,   "board header and driver disagree about the panel's CS pin");
static_assert(PIN_EPD_BUSY == PIN_DISPLAY_BUSY, "board header and driver disagree about the panel's BUSY pin");
static_assert(PIN_EPD_RST  == PIN_DISPLAY_RST,  "board header and driver disagree about the panel's RST pin");
static_assert(PIN_EPD_MOSI == DEFAULT_SDI,      "board header and driver disagree about the panel's MOSI pin");
static_assert(PIN_EPD_SCK  == DEFAULT_CLK,      "board header and driver disagree about the panel's SCK pin");
static_assert(PIN_DISPLAY_VEXT == PIN_PCB_VEXT, "board header and driver disagree about the Vext pin");

class EinkPanel : public Panel {
public:
  bool begin() override;
  bool present() const override { return _ok; }
  // The canvas, not the driver: pages get real Adafruit_GFX.
  Adafruit_GFX& gfx() override { return *_canvas; }
  void clear() override { if (_canvas) _canvas->fillScreen(0); }
  void flush(bool full) override;
  void blank(bool on) override;
  // Nothing to gain: the image stays on the glass with the power off, so the
  // display's sleep timer must not stop redrawing on our account.
  bool blanks() const override { return false; }
  const uint8_t* frame(size_t& len) const override {
    len = (size_t)((DISPLAY_WIDTH + 7) / 8) * DISPLAY_HEIGHT;
    return _canvas ? _canvas->getBuffer() : nullptr;
  }
  // On the canvas a set bit is ink; what that becomes on the glass is the
  // driver's business, decided in flush().
  uint16_t ink() const override { return 1; }
  uint16_t paper() const override { return 0; }

private:
  static bool panelAnswers();          // BUSY says whether there is one there

  // Both allocated in begin(), not held as members: a member is constructed
  // during static initialisation, before setup() runs, and the boot memory
  // bill (Diag::cost) would then attribute nearly eight kilobytes — the
  // canvas and the driver's own pagefile — to whatever happened to be
  // measured first. A board whose panel does not answer pays for neither.
  GFXcanvas1*                    _canvas = nullptr;
  EInkDisplay_WirelessPaperV1_2* _panel  = nullptr;
  // The driver treats fast mode as a mode, not a per-update flag: setting it
  // resets the controller and reloads its waveform. So it is set when it
  // changes and not before. -1 until the first flush has decided.
  int8_t _fastmode = -1;
  bool   _ok = false;
};

#endif // HAS_DISPLAY && EINK
