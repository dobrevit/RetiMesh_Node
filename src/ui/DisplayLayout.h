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
//  DisplayLayout.h — what fits on this panel, worked out once
//
//  Every page was written against one panel: 128x64, 21 columns of the 6x8
//  font, body rows at y=12/22/32/42/52, a 15x7 battery icon at the top right,
//  page dots at y=61, and a QR code given the left 62 pixels. Those numbers
//  appear in a dozen places and none of them says where they came from.
//
//  The bench is about to hold five different panels — 64x32, 128x64, a 2.13"
//  e-paper, a 2.4" LCD, and boards with none at all — so the numbers have to
//  come from somewhere rather than being repeated. This is that somewhere.
//
//  Two things are deliberately in here that are not geometry:
//
//  `refreshMs` — an OLED redraw is cheap enough to do twice a second and
//  nobody notices. E-paper costs hundreds of milliseconds and visibly flashes,
//  so a page model that assumes a redraw is free is wrong on it. The panel
//  says how often it can afford to be redrawn.
//
//  `rows` — the honest answer for a 64x32 panel is two or three, not five
//  clipped. A page that wants five rows has to be told it has three, so it can
//  choose which three rather than having the bottom two silently cut off.
// ============================================================================

#pragma once

#include <Arduino.h>
#include "Config.h"

namespace DisplayLayout {

// The built-in Adafruit_GFX font, which every page is written against.
static const uint8_t FONT_W = 6;
static const uint8_t FONT_H = 8;

struct Layout {
  uint8_t  width, height;

  // Text grid for the body of a page.
  uint8_t  columns;        // characters that fit across, at FONT_W
  uint8_t  rows;           // body rows that fit under the header
  uint8_t  rowY0;          // y of the first body row
  uint8_t  rowPitch;       // y distance between rows

  // Chrome. A small panel gives these up before it gives up a reading.
  bool     header;         // a title bar across the top
  bool     pageDots;       // the page indicator, bottom right
  uint8_t  batteryW, batteryH;   // 0 x 0 means no room for the icon

  uint8_t  headerH;        // height of the status bar, 0 where there is none
  uint8_t  iconSize;       // square, and what the bar is sized around
  bool     statusIcons;    // room in the header for the persistent state
  uint16_t refreshMs;      // how often this panel can afford a redraw
};

// 128x64: the panel every page was written for, stated rather than assumed.
constexpr Layout kOled128x64 = {
  128, 64,
  // The bar is twelve pixels rather than nine: the page dots used to own the
  // bottom four and no longer exist, so the height was going spare. Body rows
  // move down to match and the fifth still ends a pixel clear of the panel.
  /*columns*/ 21, /*rows*/ 5, /*rowY0*/ 15, /*rowPitch*/ 10,
  /*header*/ true, /*pageDots*/ false, /*batteryW*/ 7, /*batteryH*/ 10,
  // Ten, not nine: the cell is drawn a pixel taller than a square icon because
  // of its nub, so at nine the icons' floor sat a pixel above the battery's and
  // the bar looked uneven. At ten they share a floor, and the L inside the LoRa
  // icon does not move — its height is (size+1)/2, which is five either way.
  /*headerH*/ 12, /*iconSize*/ 10, /*statusIcons*/ true, /*refreshMs*/ 500,
};

// 64x32, as on the Heltec Wireless Stick: an eighth of the area. Ten columns
// and four rows, and no header bar — a title in ten characters costs a quarter
// of the panel to say something the page content already implies, and the page
// name is not worth a row when a reading could have it. No page dots either.
constexpr Layout kOled64x32 = {
  64, 32,
  /*columns*/ 10, /*rows*/ 4, /*rowY0*/ 0, /*rowPitch*/ 8,
  /*header*/ false, /*pageDots*/ false, /*batteryW*/ 0, /*batteryH*/ 0,
  /*headerH*/ 0, /*iconSize*/ 0, /*statusIcons*/ false, /*refreshMs*/ 500,
};

// Chosen at build time from the panel the board header declares. Runtime
// probing tells us a panel answered, not how big it is — an SSD1306 reports
// nothing about its own geometry — so the board is what knows.
constexpr Layout active() {
  return (DISPLAY_HEIGHT <= 32) ? kOled64x32 : kOled128x64;
}

// Longest string that fits a full-width row, excluding the terminator. Pages
// size their buffers from this rather than from a constant that happens to
// suit one panel.
constexpr uint8_t textWidth() { return active().columns; }

// True where the panel is too small for the full page set to mean anything.
// A page asked this decides what to show rather than being clipped: ten
// columns will not hold "RSSI -104  SNR 11.5", and showing the left half of it
// is worse than showing one of the two numbers whole.
constexpr bool compact() { return DISPLAY_COMPACT != 0; }

// y of a body row, clamped so a page that asks for more rows than exist draws
// over the last one rather than off the bottom of the panel.
inline uint8_t rowY(uint8_t row) {
  const Layout l = active();
  const uint8_t r = row < l.rows ? row : (uint8_t)(l.rows - 1);
  return (uint8_t)(l.rowY0 + r * l.rowPitch);
}

} // namespace DisplayLayout
