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
//  The panel this firmware's display abstraction was written for. Three things
//  about it are unlike the OLED, and they are why the abstraction exists:
//
//  An update costs. A full refresh takes the better part of a second and
//  flashes the panel black-white-black while it runs; a partial one is faster
//  and quiet but leaves a faint record of what was there before. So the page
//  is drawn into memory on every pass and RefreshPolicy decides what reaches
//  the glass — nothing when the frame is unchanged, a partial update when it
//  is, and a full one every few partials to clear the ghosting they leave.
//
//  It has an off switch that is not the picture. The image survives with no
//  power at all, so "blank" cannot mean clearing the glass — but the
//  controller driving it does not stop when the picture does, and it has a
//  deep sleep with exactly one way out. Where that is spent, and what the
//  update after it owes, is EinkSleep.h and the comment on blank().
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

#include "EinkSleep.h"

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

// The driver's panel, plus the one command it does not offer. Everything that
// reaches the controller — sendCommand, sendData — is protected on the
// library's base class, and the only public power call it has, customPowerOff,
// is compiled out on this board (it is wrapped in `#ifndef ALL_IN_ONE`, and
// the library's own platform header for the Wireless Paper sets ALL_IN_ONE
// true). So the deep sleep is reached the way the library intends a display to
// be extended: by deriving one.
//
// The command and its parameter are the panel's own: R10h with A[1:0] = 01,
// "Enter Deep Sleep Mode 1", which is also the last line of the datasheet's
// reference operating sequence — load image, master activation, wait for BUSY
// low, `Command 0x10  Data 0x01`, power off. No settling wait is specified for
// it and none is invented here: the part raises BUSY and stops listening, and
// the next thing it is given is a reset.
class EinkDriver : public EInkDisplay_WirelessPaperV1_2 {
public:
  // Safe to send only with BUSY already low, which every path here satisfies:
  // the driver's update() ends in its own wait(), and the panel is touched
  // from one task.
  void deepSleep() {
    sendCommand(0x10);
    sendData(0x01);
  }
};

class EinkPanel : public Panel {
public:
  bool begin() override;
  bool present() const override { return _ok; }
  // The canvas, not the driver: pages get real Adafruit_GFX.
  Adafruit_GFX& gfx() override { return *_canvas; }
  void clear() override { if (_canvas) _canvas->fillScreen(0); }
  void flush(bool full) override;
  void blank(bool on) override;
  // False, and it stays false now that blanking does something. This is the
  // question the display's *sleep timer* asks before it stops redrawing, and
  // on a panel that holds its image the answer has not changed: a node left
  // on a shelf must keep showing live readings rather than the ones it had an
  // hour ago, and that is worth far more than the microamps the timer could
  // save. A blank somebody asked for is a different question and reaches
  // blank() regardless of this.
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
  GFXcanvas1* _canvas = nullptr;
  EinkDriver* _panel  = nullptr;
  // The driver treats fast mode as a mode, not a per-update flag: setting it
  // resets the controller and reloads its waveform. So it is set when it
  // changes and not before. -1 until the first flush has decided.
  int8_t _fastmode = -1;
  // Whether the controller is listening, and what the next update owes it if
  // it is not. The sequence is EinkSleep's, so that "a wake costs a reset and
  // a whole frame" is stated once and pinned on the host rather than being
  // spread across blank() and flush().
  EinkSleep _sleep;
  bool   _ok = false;
};

#endif // HAS_DISPLAY && EINK
