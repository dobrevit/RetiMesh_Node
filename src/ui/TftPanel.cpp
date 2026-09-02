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

void TftPanel::window(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  // CASET/RASET take big-endian start and end, inclusive.
  const uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                          (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
  const uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                          (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
  cmd(CASET, ca, sizeof(ca));
  cmd(RASET, ra, sizeof(ra));
}

void TftPanel::blitArea(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const uint8_t* px) {
  if (!_ok) return;
  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  window(x1, y1, x2, y2);
  cmd(RAMWR);
  digitalWrite(PIN_TFT_DC, HIGH);
  _spi.writeBytes(px, (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1) * 2);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();
  if (!_lit) { _lit = true; applyBacklight(); }
}

void TftPanel::applyBacklight() {
  if (!_lit || _blanked) return;
  ledcWrite(PIN_TFT_BL, (uint32_t)_brightPct * 255u / 100u);
}

void TftPanel::setBrightness(uint8_t pct) {
  _brightPct = pct > 100 ? 100 : pct;
  applyBacklight();
}

void TftPanel::setRotation(uint8_t quarterTurns) {
  if (!_ok) return;
  // MV swaps the axes, MX/MY mirror them — the standard four for an ST7789
  // whose RAM is exactly the glass, so no window offsets appear. With MV set
  // the controller reads CASET as the long axis by itself, which is why
  // blitArea needs no help: callers simply address the turned frame.
  static constexpr uint8_t kMad[4] = { 0x00, 0x60, 0xC0, 0xA0 };
  const uint8_t m = kMad[quarterTurns & 3];
  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(MADCTL, &m, 1);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();
}

bool TftPanel::begin() {
  // The panel sits behind the switched peripheral rail, active low, like
  // every panel on a Heltec board.
  panelVextOn();

  _canvas = new (std::nothrow) GFXcanvas1(kW, kH);
  _shadow = (uint8_t*)calloc(((size_t)kW + 7) / 8 * kH, 1);
  if (!_canvas || !_canvas->getBuffer() || !_shadow) {
    log_e("tft: no room for a %dx%d canvas — display disabled", kW, kH);
    delete _canvas; _canvas = nullptr;
    free(_shadow);  _shadow = nullptr;
    return false;
  }

  pinMode(PIN_TFT_CS, OUTPUT);  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_TFT_DC, OUTPUT);  digitalWrite(PIN_TFT_DC, HIGH);
  // PWM rather than a switch: brightness is a setting now. 20 kHz keeps the
  // dimming above anything a camera or an ear could catch.
  ledcAttach(PIN_TFT_BL, 20000, 8);
  ledcWrite(PIN_TFT_BL, 0);                                       // dark until there is a frame

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

void TftPanel::flush(bool full) {
  if (!_ok) return;

  const uint8_t* fb = _canvas->getBuffer();
  const size_t stride = ((size_t)kW + 7) / 8;

  // The band of canvas rows that changed since the glass last saw them. A
  // ticking counter or the charge sweep touches a row or two; streaming the
  // other three hundred panel lines for it was most of a core's percent
  // spent repeating what the controller's RAM already holds.
  int16_t y0 = 0, y1 = kH - 1;
  if (!full) {
    while (y0 < kH && memcmp(fb + y0 * stride, _shadow + y0 * stride, stride) == 0) y0++;
    if (y0 == kH) {                       // nothing changed at all
      if (!_lit) { _lit = true; applyBacklight(); }
      return;
    }
    while (y1 > y0 && memcmp(fb + y1 * stride, _shadow + y1 * stride, stride) == 0) y1--;
  }

  // Ink white on black, as every page draws; each canvas nibble becomes 16
  // bytes of doubled RGB565 through a table rather than four branches per
  // pixel. GFXcanvas1 packs pixels MSB-first within each byte.
  static const uint8_t* lut = [] {
    static uint8_t t[16][16];
    for (int n = 0; n < 16; n++)
      for (int px = 0; px < 4; px++) {
        const uint8_t v = (n & (8 >> px)) ? 0xFF : 0x00;
        memset(&t[n][px * 4], v, 4);
      }
    return &t[0][0];
  }();

  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  window(0, (int16_t)(y0 * 2), (int16_t)(DISPLAY_WIDTH - 1), (int16_t)(y1 * 2 + 1));
  cmd(RAMWR);
  digitalWrite(PIN_TFT_DC, HIGH);

  uint8_t line[DISPLAY_WIDTH * 2];
  for (int16_t y = y0; y <= y1; y++) {
    const uint8_t* row = fb + (size_t)y * stride;
    uint8_t* out = line;
    for (size_t b = 0; b < stride; b++) {
      memcpy(out, lut + (row[b] >> 4) * 16, 16);  out += 16;
      memcpy(out, lut + (row[b] & 0x0F) * 16, 16); out += 16;
    }
    _spi.writeBytes(line, sizeof(line));            // the row, doubled across...
    _spi.writeBytes(line, sizeof(line));            // ...and down
  }
  memcpy(_shadow + (size_t)y0 * stride, fb + (size_t)y0 * stride,
         (size_t)(y1 - y0 + 1) * stride);

  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();

  // The first frame is on the glass; only now is the backlight worth its
  // current. Before this the panel shows the controller's power-on noise.
  if (!_lit) { _lit = true; applyBacklight(); }
}

void TftPanel::blank(bool on) {
  if (!_ok) return;
  _spi.beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(on ? DISPOFF : DISPON);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi.endTransaction();
  _blanked = on;
  if (on) ledcWrite(PIN_TFT_BL, 0); else applyBacklight();
  _lit = !on;
}

#endif // HAS_DISPLAY && TFT
