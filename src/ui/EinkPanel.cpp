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

bool EinkPanel::begin() {
  // The panel sits on the switched peripheral rail, active low, as the OLED
  // boards' panels do. The driver drives it itself on an all-in-one board,
  // but it is brought up here as well so the rail is settled before the
  // first transfer rather than during it.
  pinMode(PIN_DISPLAY_VEXT, OUTPUT);
  digitalWrite(PIN_DISPLAY_VEXT, LOW);
  delay(50);

  _canvas = new (std::nothrow) GFXcanvas1(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!_canvas || !_canvas->getBuffer()) {
    log_e("e-paper: no room for a %dx%d frame — display disabled", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    delete _canvas; _canvas = nullptr;
    return false;
  }
  _panel.begin();
  _panel.landscape();                  // 250 across, 122 down, as the layout says
  _canvas->fillScreen(0);
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
  if (full) _panel.fastmodeOff();
  else      _panel.fastmodeOn();
  _panel.clearMemory();
  _panel.drawBitmap(0, 0, _canvas->getBuffer(), DISPLAY_WIDTH, DISPLAY_HEIGHT, BLACK);
  _panel.update();
}

void EinkPanel::blank(bool on) {
  // Nothing to switch off: an e-paper panel holds its image without power,
  // which is most of the point of one. Blanking it would mean clearing the
  // glass — throwing away the reading a passer-by is meant to be able to
  // read on a node that is asleep — so the panel is simply left showing what
  // it last showed, and the display task stops drawing.
  (void)on;
}

#endif // HAS_DISPLAY && EINK
