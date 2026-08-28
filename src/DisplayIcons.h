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
//  DisplayIcons.h — the glyphs, drawn rather than stored
//
//  A label that never changes is the worst use of a column. "RX" and "TX" cost
//  two characters each to say something the reader learns once and then never
//  needs again; on a ten-column panel that is a fifth of the row spent on
//  letters instead of digits.
//
//  Two rules decide what belongs here, because the alternative — a glyph for
//  everything — is worse than the text it replaces:
//
//  1. Only symbols the reader already knows. Arrows for direction, arcs for
//     Wi-Fi, bars for strength, a cell for the battery. There is no recognised
//     icon for "paths" or "spreading factor", and an invented one cannot be
//     decoded on a panel with no room for a legend.
//
//  2. An icon replaces the label, never the value. A bar chart with no number
//     beside it says nothing, which is why signal strength was kept out of the
//     header when these pages were designed. That decision stands: these go
//     next to figures, not instead of them.
//
//  Everything is drawn from primitives and takes a size, so one definition
//  serves a 64x32 OLED at six pixels and a 2.4" LCD at sixteen. Nothing is
//  stored as a bitmap, so nothing costs flash per panel. And they take
//  Adafruit_GFX rather than Adafruit_SSD1306, because the e-paper and TFT
//  panels arriving this week are GFX too.
// ============================================================================

#pragma once

#include <Adafruit_GFX.h>

namespace DisplayIcons {

// Direction of traffic. The arrowhead is the whole icon at small sizes — a
// stem below six pixels is a single pixel and reads as noise.
inline void arrow(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t size, bool up,
                  uint16_t color) {
  const int16_t h = size, w = size;
  const int16_t midX = x + w / 2;
  if (up) {
    g.drawLine(midX, y, x, y + h / 2, color);
    g.drawLine(midX, y, x + w - 1, y + h / 2, color);
    if (h >= 6) g.drawLine(midX, y, midX, y + h - 1, color);
  } else {
    g.drawLine(midX, y + h - 1, x, y + h / 2, color);
    g.drawLine(midX, y + h - 1, x + w - 1, y + h / 2, color);
    if (h >= 6) g.drawLine(midX, y, midX, y + h - 1, color);
  }
}

// The Wi-Fi fan: a dot with arcs above it, as many as the uplink deserves.
//
// `bars` is 1..3 for a link that is up and NOT_JOINED for one that is
// configured and down. Zero is accepted and draws the dot alone, but callers
// should not use it for a live link: the first version did, and a lone dot
// reads as neither "no signal" nor "Wi-Fi" but as a stray pixel — the first
// person to see one had to ask what it was. A connected link always deserves
// at least one arc, and a cross above the dot says the uplink is meant to
// exist and does not, in three pixels.
//
// No station configured at all draws nothing: there is no uplink to report on,
// and an icon for a feature nobody has asked for is noise.
static const uint8_t WIFI_NOT_JOINED = 0xFF;

inline void wifi(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t size,
                 uint8_t bars, uint16_t color) {
  const int16_t midX = x + size / 2;
  const int16_t base = y + size - 1;
  g.drawPixel(midX, base, color);

  if (bars == WIFI_NOT_JOINED) {
    const int16_t cy = base - 3;
    g.drawLine(midX - 1, cy - 1, midX + 1, cy + 1, color);
    g.drawLine(midX - 1, cy + 1, midX + 1, cy - 1, color);
    return;
  }
  // Arcs as widening chevrons: a true arc is indistinguishable from one at
  // this size, and a chevron survives being three pixels tall. Both spans are
  // derived from the size so the third and widest chevron lands exactly on the
  // edges of the box — written the obvious way it grew by a fixed step and the
  // outer chevron drew two pixels outside its slot on each side, into whatever
  // was next to it.
  for (uint8_t i = 1; i <= 3 && i <= bars; i++) {
    const int16_t dx = (int16_t)(i * ((size - 1) / 2) / 3);
    const int16_t dy = base - (int16_t)(i * (size - 1) / 3);
    if (dy < y) break;
    g.drawLine(midX - dx, dy + dx / 2, midX, dy, color);
    g.drawLine(midX, dy, midX + dx, dy + dx / 2, color);
  }
}

// LoRa strength: the bars, with an L tucked into the space they leave.
//
// Ascending bars fill the lower right of their box and leave the upper left
// empty, so the label costs nothing — it sits inside the icon rather than
// beside it, and the whole thing is one square instead of a glyph and a graph
// with a gap between them that read as two things.
//
// The L is needed because bars alone say something is strong without saying
// what, and there is a Wi-Fi fan in the same bar they could as easily belong
// to. It clears every bar at any size the panels here use, checked rather than
// eyeballed: the diagonal the bars form is what makes the room.
// Ascending bars with a floor: every column keeps a lit pixel on the bottom
// row whatever the strength, separated from the bar above it by a blank row.
//
// That floor is the point. Bars alone go blank when there is no signal and the
// icon disappears, which reads as "nothing here" rather than "nothing heard" —
// the scale itself should stay visible so the empty bars mean something. It is
// how a phone draws it, and it is right.
//
// Columns are spaced exactly two pixels apart rather than spread across the
// width, because a width that does not divide evenly put the last column three
// pixels from its neighbour and one column looked detached from the rest.
// Width and height are separate for the same reason: the icon wants nine
// across for five columns and ten down to floor level with the cell beside it.
inline void bars(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t w, uint8_t h,
                 uint8_t pct, uint16_t color) {
  const uint8_t n = (uint8_t)((w + 1) / 2);        // one column every two pixels
  if (n == 0 || h < 4) return;
  const int16_t floorY = y + h - 1;
  const int16_t barBase = floorY - 2;              // a blank row above the floor
  const int16_t barRows = (int16_t)h - 2;
  const uint8_t step = 100 / n;

  for (uint8_t i = 0; i < n; i++) {
    const int16_t bx = x + i * 2;
    g.drawPixel(bx, floorY, color);                // the scale, always
    if (pct <= i * step) continue;
    // At least one pixel. Where the height did not divide by the column count
    // the shortest bar rounded to zero, and drawLine then ran from the baseline
    // to a row *below* it — a stray two-pixel mark that merged with the floor
    // and read as a leading vertical line belonging to nothing.
    int16_t tall = (int16_t)((i + 1) * barRows / n);
    if (tall < 1) tall = 1;
    g.drawLine(bx, barBase, bx, barBase - tall + 1, color);
  }
}

// LoRa strength: the bars, with an L tucked into the space they leave.
//
// Ascending bars fill the lower right of their box and leave the upper left
// empty, so the label costs nothing — it sits inside the icon rather than
// beside it, and the whole thing is one square instead of a glyph and a graph
// with a gap between them that read as two things.
//
// The L is needed because bars alone say something is strong without saying
// what, and there is a Wi-Fi fan in the same bar they could as easily belong
// to. It clears every bar at any size the panels here use, checked rather than
// eyeballed: the diagonal the bars form is what makes the room.
inline void loraSignal(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t w,
                       uint8_t h, uint8_t pct, uint16_t color) {
  bars(g, x, y, w, h, pct, color);
  const int16_t lh = (h + 1) / 2;
  if (lh < 3) return;
  g.drawLine(x, y, x, y + lh - 1, color);              // the upright
  g.drawLine(x, y + lh - 1, x + 2, y + lh - 1, color); // the foot
}

// The cell stood on end. A horizontal one is seventeen pixels of a hundred and
// twenty-eight; upright it is seven, and the header has the height to spare now
// that the page dots have gone. Fills from the bottom, as a tank does.
inline void batteryVertical(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t w,
                            uint8_t h, uint8_t pct, bool charging, uint8_t sweep,
                            uint16_t color, uint16_t bg) {
  g.drawLine(x + w / 2 - 1, y, x + w / 2 + 1, y, color);   // the nub, on top
  g.drawRect(x, y + 1, w, h - 1, color);

  // A solid column, not a stack of lines with gaps between them. At seven
  // pixels wide the gaps were most of the icon and the fill was three pixels
  // of it, which read as a texture rather than a level. One pixel of margin
  // inside the border is enough to keep the two apart.
  const int16_t inner = (int16_t)h - 3;          // usable height inside the border
  if (inner <= 0) return;
  const int16_t lit = (int16_t)((uint32_t)pct * inner / 100);
  if (lit > 0) {
    // Filled from the bottom, as a tank is. Drawn the other way round a nearly
    // flat cell would look nearly full, which is the one reading that has to
    // be right.
    g.fillRect(x + 1, y + h - 1 - lit, w - 2, lit, color);
  }
  // While charging a line travels up the cell, so one sitting at 100% still
  // reads as charging rather than looking identical to a full idle one. It is
  // drawn in the background colour where there is fill under it and the
  // foreground where there is not, which is why the caller has to say what the
  // background is: inverting the value only happens to work on a one-bit
  // panel, and the TFT arriving this week is not one.
  if (charging && inner >= 3) {
    const int16_t band = y + h - 2 - (int16_t)((uint32_t)sweep * (inner - 1) / 100);
    const bool overFill = band >= y + h - 1 - lit;
    g.drawLine(x + 1, band, x + w - 2, band, overFill ? bg : color);
  }
}

// A dish, because the previous glyph — a dot with a diagonal through it — was
// asked about rather than recognised, and a dish is the one shape people
// already associate with receiving from something overhead. Drawn facing up and
// to the right: the reflector as an arc, the feed at its focus, a mast and a
// base. With a fix, two ticks come off the feed; without, the dish is empty,
// which is the difference between pointed at the sky and hearing nothing.
inline void dish(Adafruit_GFX& g, int16_t x, int16_t y, uint8_t size, bool fix,
                 uint16_t color) {
  const int16_t r = (int16_t)(size / 2);
  const int16_t cx = x + size - 2, cy = y + r;
  g.drawCircleHelper(cx, cy, r, 0x8 | 0x4, color);          // the reflector
  g.drawPixel(cx - r / 2, cy, color);                       // the feed horn
  g.drawLine(cx - 1, cy + r / 2, cx - 1, y + size - 1, color);          // mast
  g.drawLine(cx - 3, y + size - 1, cx + 1, y + size - 1, color);        // base
  if (fix) {
    g.drawPixel(cx - r / 2 + 2, cy - 2, color);
    g.drawPixel(cx - r / 2 + 3, cy - 3, color);
  }
}

} // namespace DisplayIcons
