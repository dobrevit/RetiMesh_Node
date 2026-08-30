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
//  EinkPanel.cpp — see EinkPanel.h
// ============================================================================
#include "EinkPanel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_EINK

#include <new>

// How long a panel may take to drop BUSY after a reset before we conclude
// there is nothing on the other end of the ribbon.
static constexpr uint32_t kBusyTimeoutMs = 2000;

// The driver waits on BUSY with no timeout of its own — a panel that never
// answers would spin inside its begin() forever, and that call sits in
// setup() before Wi-Fi, the radio, the web server and the console. So the
// line is asked first, the way OledPanel probes the I2C bus before handing
// the SSD1306 driver an address: a board with no panel has to come up
// headless and say so, not hang where nobody can see why.
//
// An absent panel leaves BUSY floating, so it is pulled up and reads high; a
// panel that is there drives it high while it wakes from reset and low when
// it is ready.
bool EinkPanel::panelAnswers() {
  pinMode(PIN_EPD_RST, OUTPUT);
  pinMode(PIN_EPD_BUSY, INPUT_PULLUP);
  digitalWrite(PIN_EPD_RST, LOW);
  delay(10);
  digitalWrite(PIN_EPD_RST, HIGH);
  const uint32_t deadline = millis() + kBusyTimeoutMs;
  while (digitalRead(PIN_EPD_BUSY) == HIGH && (int32_t)(millis() - deadline) < 0) delay(5);
  return digitalRead(PIN_EPD_BUSY) == LOW;
}

bool EinkPanel::begin() {
  // The panel sits on the switched peripheral rail, active low, as the OLED
  // boards' panels do, and it has to be up before the line below means
  // anything.
  pinMode(PIN_DISPLAY_VEXT, OUTPUT);
  digitalWrite(PIN_DISPLAY_VEXT, LOW);
  delay(50);

  if (!panelAnswers()) {
    log_w("e-paper: BUSY never went low after reset (pin %d) — no panel, running headless",
          PIN_EPD_BUSY);
    return false;
  }

  // The driver first, because its constructor takes the larger allocation and
  // does it with a bare new[] it does not check; giving it the roomier heap
  // is the only influence we have over that. The canvas is ours and is
  // checked.
  _panel = new (std::nothrow) EInkDisplay_WirelessPaperV1_2();
  _canvas = new (std::nothrow) GFXcanvas1(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!_panel || !_canvas || !_canvas->getBuffer()) {
    log_e("e-paper: no room for a %dx%d frame — display disabled", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    delete _canvas; _canvas = nullptr;
    delete _panel;  _panel  = nullptr;
    return false;
  }

  _panel->begin();
  _panel->landscape();                 // 250 across, 122 down, as the layout says
  // White is the background the pages assume, and setting it here also means
  // every later call clears the page to it rather than to black.
  _panel->setBackgroundColor(WHITE);

  // The same invariants OledPanel establishes on its driver, on the surface
  // the pages actually draw on: a row one character too long is clipped, not
  // wrapped onto the reading below it.
  _canvas->setTextWrap(false);
  _canvas->setTextColor(ink());
  _canvas->fillScreen(paper());

  _ok = true;
  log_i("e-paper %dx%d up (CS %d, DC %d, RST %d, BUSY %d)",
        DISPLAY_WIDTH, DISPLAY_HEIGHT, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY);
  return _ok;
}

void EinkPanel::flush(bool full) {
  if (!_ok) return;
  // Partial by default because a full pass flashes the panel; full when the
  // policy says the ghosting has had long enough, and on the first frame,
  // where what is on the glass is the previous firmware's.
  //
  // Set only when it changes: each of these resets the controller, reloads
  // its waveform and waits on BUSY twice, which is not a thing to do before
  // every update on a panel whose updates are what we are rationing.
  const int8_t want = full ? 0 : 1;
  if (want != _fastmode) {
    if (full) _panel->fastmodeOff();
    else      _panel->fastmodeOn();
    _fastmode = want;
  }
  // Clear the page in memory, not on the glass. The obvious call for this,
  // clearMemory(), also pushes the blank frame to the controller a byte at a
  // time — four thousand SPI transactions immediately overwritten by the real
  // frame below. setBackgroundColor does the same local clear and no I/O.
  _panel->setBackgroundColor(WHITE);
  _panel->drawBitmap(0, 0, _canvas->getBuffer(), DISPLAY_WIDTH, DISPLAY_HEIGHT, BLACK);
  _panel->update();
}

void EinkPanel::blank(bool on) {
  // Nothing to switch off: an e-paper panel holds its image without power,
  // which is most of the point of one. Blanking it would mean clearing the
  // glass — throwing away the reading a passer-by is meant to be able to
  // take off a sleeping node — so the panel keeps showing what it last
  // showed. blanks() tells the display's sleep timer not to bother.
  (void)on;
}

#endif // HAS_DISPLAY && EINK
