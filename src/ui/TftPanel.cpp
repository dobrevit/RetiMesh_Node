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
//  TftPanel.cpp — see TftPanel.h
// ============================================================================
#include "TftPanel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_TFT

#include <new>

// The ST7789 commands this driver speaks. Names from the datasheet.
namespace {
constexpr uint8_t SWRESET = 0x01;
constexpr uint8_t SLPOUT  = 0x11;
constexpr uint8_t NORON   = 0x13;
constexpr uint8_t INVON   = 0x21;
constexpr uint8_t DISPOFF = 0x28;
constexpr uint8_t DISPON  = 0x29;
constexpr uint8_t CASET   = 0x2A;
constexpr uint8_t RASET   = 0x2B;
constexpr uint8_t RAMWR   = 0x2C;
constexpr uint8_t MADCTL  = 0x36;
constexpr uint8_t COLMOD  = 0x3A;
}

void TftPanel::cmd(uint8_t c) { cmd(c, nullptr, 0); }

void TftPanel::cmd(uint8_t c, const uint8_t* data, size_t len) {
  digitalWrite(PIN_TFT_DC, LOW);          // command
  _spi.write(c);
  if (len) {
    digitalWrite(PIN_TFT_DC, HIGH);       // ... and its parameters
    _spi.writeBytes(data, len);
  }
}

void TftPanel::window() {
  // The whole panel. CASET/RASET take big-endian start and end, inclusive.
  const uint8_t ca[4] = { 0, 0, (uint8_t)((DISPLAY_WIDTH - 1) >> 8),  (uint8_t)((DISPLAY_WIDTH - 1) & 0xFF) };
  const uint8_t ra[4] = { 0, 0, (uint8_t)((DISPLAY_HEIGHT - 1) >> 8), (uint8_t)((DISPLAY_HEIGHT - 1) & 0xFF) };
  cmd(CASET, ca, sizeof(ca));
  cmd(RASET, ra, sizeof(ra));
}

bool TftPanel::begin() {
  // The panel sits behind the switched peripheral rail, active low, like
  // every panel on a Heltec board.
#if HAS_DISPLAY_VEXT
  pinMode(PIN_DISPLAY_VEXT, OUTPUT);
  digitalWrite(PIN_DISPLAY_VEXT, LOW);
  delay(50);
#endif

  _canvas = new (std::nothrow) GFXcanvas1(kW, kH);
  if (!_canvas || !_canvas->getBuffer()) {
    log_e("tft: no room for a %dx%d canvas — display disabled", kW, kH);
    delete _canvas; _canvas = nullptr;
    return false;
  }

  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);  digitalWrite(PIN_TFT_DC, HIGH);
  pinMode(PIN_TFT_BL, OUTPUT);  digitalWrite(PIN_TFT_BL, LOW);   // dark until there is a frame

  // Hardware reset: low for a moment, then the controller wants 120 ms
  // before it will take SLPOUT seriously.
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, HIGH); delay(5);
  digitalWrite(PIN_TFT_RST, LOW);  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH); delay(120);

  _spi.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);

  cmd(SLPOUT);
  delay(120);
  const uint8_t fmt16 = 0x55;             // RGB565, the panel's native 16 bits
  cmd(COLMOD, &fmt16, 1);
  const uint8_t portrait = 0x00;          // row/column order as the layout assumes
  cmd(MADCTL, &portrait, 1);
  // ST7789 glass on these modules is fitted inverted; without this white is
  // black and the "dark" panel glows.
  cmd(INVON);
  cmd(NORON);
  cmd(DISPON);

  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();

  _canvas->setTextWrap(false);
  _canvas->setTextColor(ink());
  _canvas->fillScreen(paper());

  _ok = true;
  log_i("tft %dx%d up, drawing at %dx%d (CS %d, DC %d, RST %d, BL %d)",
        DISPLAY_WIDTH, DISPLAY_HEIGHT, kW, kH,
        PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST, PIN_TFT_BL);
  return _ok;
}

void TftPanel::flush(bool) {
  if (!_ok) return;

  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  window();
  cmd(RAMWR);
  digitalWrite(PIN_TFT_DC, HIGH);

  // One canvas row becomes two panel lines of doubled pixels. The line buffer
  // is built once per canvas row and written twice — 480 bytes on the stack,
  // 320 writes per frame, which at 40 MHz is a few tens of milliseconds.
  const uint8_t* fb = _canvas->getBuffer();
  const size_t stride = (kW + 7) / 8;
  uint8_t line[DISPLAY_WIDTH * 2];
  for (int16_t y = 0; y < kH; y++) {
    const uint8_t* row = fb + (size_t)y * stride;
    for (int16_t x = 0; x < kW; x++) {
      // Ink white on black, as every page draws. GFXcanvas1 packs pixels
      // MSB-first within each byte.
      const bool on = row[x >> 3] & (0x80 >> (x & 7));
      const uint8_t v = on ? 0xFF : 0x00;
      line[x * 4 + 0] = v; line[x * 4 + 1] = v;    // pixel doubled across...
      line[x * 4 + 2] = v; line[x * 4 + 3] = v;
    }
    _spi.writeBytes(line, sizeof(line));            // ...and down
    _spi.writeBytes(line, sizeof(line));
  }

  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();

  // The first frame is on the glass; only now is the backlight worth its
  // current. Before this the panel shows the controller's power-on noise.
  if (!_lit) { digitalWrite(PIN_TFT_BL, HIGH); _lit = true; }
}

void TftPanel::blank(bool on) {
  if (!_ok) return;
  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(on ? DISPOFF : DISPON);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();
  digitalWrite(PIN_TFT_BL, on ? LOW : HIGH);
  _lit = !on;
}

#endif // HAS_DISPLAY && TFT
