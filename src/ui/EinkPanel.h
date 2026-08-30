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
//  is, and a full one periodically to clear the ghosting.
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

class EinkPanel : public Panel {
public:
  bool begin() override;
  bool present() const override { return _ok; }
  // The canvas, not the driver: pages get real Adafruit_GFX.
  Adafruit_GFX& gfx() override { return *_canvas; }
  void clear() override { if (_canvas) _canvas->fillScreen(0); }
  void flush(bool full) override;
  void blank(bool on) override;
  const uint8_t* frame(size_t& len) const override {
    len = (size_t)((DISPLAY_WIDTH + 7) / 8) * DISPLAY_HEIGHT;
    return _canvas ? _canvas->getBuffer() : nullptr;
  }
  // On the canvas a set bit is ink; what that becomes on the glass is the
  // driver's business, decided in flush().
  uint16_t ink() const override { return 1; }
  uint16_t paper() const override { return 0; }

private:
  // Allocated in begin(), not held as a member: a member is constructed
  // during static initialisation, before setup() runs, and the boot memory
  // bill (Diag::cost) would then attribute nearly four kilobytes of canvas to
  // whatever happened to be measured first. A board whose panel does not
  // start does not pay for it at all this way, which is how the SSD1306
  // driver behaves too — it allocates inside its own begin().
  GFXcanvas1*       _canvas = nullptr;
  EInkDisplay_WirelessPaperV1_2 _panel;
  bool _ok = false;
};

#endif // HAS_DISPLAY && EINK
