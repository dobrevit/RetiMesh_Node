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
#include "SpiBus.h"

// The ST7789 commands this driver speaks. Names from the datasheet.
namespace {
constexpr uint8_t SLPOUT  = 0x11;
constexpr uint8_t NORON   = 0x13;
constexpr uint8_t INVOFF  = 0x20;
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
  _spi->write(c);
  if (len) {
    digitalWrite(PIN_TFT_DC, HIGH);       // ... and its parameters
    _spi->writeBytes(data, len);
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
  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  window(x1, y1, x2, y2);
  cmd(RAMWR);
  digitalWrite(PIN_TFT_DC, HIGH);
  _spi->writeBytes(px, (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1) * 2);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();
  if (!_lit) { _lit = true; applyBacklight(); }
}

// ---------------------------------------------------------------------------
// The backlight
// ---------------------------------------------------------------------------
#if BACKLIGHT_KIND == BACKLIGHT_KIND_AW9364

// A one-wire dimmer, not an LED on a gate. Brightness is a counter inside the
// part: holding the line high turns it on at full, and each further low-high
// pulse steps it down one of sixteen levels, wrapping round from the bottom
// back to the top. Taking the line low for a few milliseconds turns it off and
// forgets the count.
//
// So the part has state we cannot read, and the only way to reach a level is
// to count pulses from the one we believe it is on — which is why the current
// level is remembered here. It is the reason a PWM channel does not work: at
// any useful frequency it sends thousands of steps a second and lands wherever
// the wrap leaves it. That still lights the panel, which is how the mistake
// survives in firmware that makes it; it is not a dimmer.
namespace {
constexpr uint8_t kSteps = 16;            // the part's whole range
uint8_t sLevel = 0;                       // 0 = off, 1..16 = the counter
}

void TftPanel::backlightBegin() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);          // off, and the counter forgotten
  // Held, because the part only forgets its counter after the low it treats as
  // "off" — half a millisecond. A reset that leaves the rail up leaves the
  // dimmer lit and counting, and a shorter pulse than this would take sLevel =
  // 0 as fact while the part sat on whatever level it was on: the first frame
  // would then step from a level nothing is at and come up at the wrong one.
  delay(3);
  sLevel = 0;
}

void TftPanel::backlightSet(uint8_t pct) {
  // Sixteen levels, and never round a lit panel down to off: a caller asking
  // for 1 % wants the dimmest light, not darkness. Only an explicit zero is
  // off, which is what blank() asks for.
  uint8_t want = pct == 0 ? 0 : (uint8_t)((pct * kSteps + 99) / 100);
  if (want > kSteps) want = kSteps;

  if (want == 0) {
    digitalWrite(PIN_TFT_BL, LOW);
    delay(3);                             // the part's own off time
    sLevel = 0;
    return;
  }
  if (sLevel == 0) {                      // from dark: on at full, then step down
    digitalWrite(PIN_TFT_BL, HIGH);
    delayMicroseconds(30);
    sLevel = kSteps;
  }
  if (want == sLevel) return;
  // Pulses only ever step downwards, so reaching a brighter level means going
  // round the wrap — the modulo is that trip, and it is why this is counted
  // rather than written.
  const uint8_t from = kSteps - sLevel, to = kSteps - want;
  const uint8_t pulses = (uint8_t)((kSteps + to - from) % kSteps);
  for (uint8_t i = 0; i < pulses; i++) {
    // Both edges held. The part wants at least half a microsecond either side
    // and treats a low longer than half a millisecond as "off", so the window
    // is wide but it is not "whatever two digitalWrite calls happen to take" —
    // and a step the part declines to count is one this side has no way to
    // notice, because the counter it is tracking cannot be read back.
    digitalWrite(PIN_TFT_BL, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TFT_BL, HIGH);
    delayMicroseconds(2);
  }
  sLevel = want;
}

#else   // BACKLIGHT_KIND_PWM

void TftPanel::backlightBegin() {
  // PWM rather than a switch: brightness is a setting now. 20 kHz keeps the
  // dimming above anything a camera or an ear could catch.
  ledcAttach(PIN_TFT_BL, 20000, 8);
  backlightSet(0);                        // dark, whichever way round the pin is
}

void TftPanel::backlightSet(uint8_t pct) {
  const uint32_t duty = (uint32_t)pct * 255u / 100u;
  // Some boards sink the LED's return rather than driving its gate, so the
  // pin is low to light it and the duty cycle runs the other way.
  ledcWrite(PIN_TFT_BL, BACKLIGHT_ACTIVE_LOW ? 255u - duty : duty);
}

#endif

void TftPanel::applyBacklight() {
  if (!_lit || _blanked) return;
  backlightSet(_brightPct);
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
  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(MADCTL, &m, 1);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();
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
  backlightBegin();                       // dark until there is a frame

  // Hardware reset: low for a moment, then the controller wants 120 ms
  // before it will take SLPOUT seriously. Some boards do not give the panel a
  // reset line of its own — it is tied to the board's, so the controller comes
  // out of reset with the MCU and there is nothing here to pulse. Those wait
  // anyway, because the settling time is the controller's either way.
#if PIN_TFT_RST >= 0
  pinMode(PIN_TFT_RST, OUTPUT);
  digitalWrite(PIN_TFT_RST, HIGH); delay(5);
  digitalWrite(PIN_TFT_RST, LOW);  delay(20);
  digitalWrite(PIN_TFT_RST, HIGH); delay(120);
#else
  delay(120);
#endif

  _spi = &SpiBus::get(TFT_SPI_BUS, PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI);
  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);

  // A software reset was tried here for the board that has no reset line, on
  // the reasoning that a controller still carrying the previous firmware's
  // registers is not one this driver's six commands can describe. The bench
  // disagreed twice: the reset alone left a lit backlight and no picture at
  // all, and supplying the standard ST7789 power and gamma block alongside it
  // made the glass darker still. Both are worse than the state they replaced,
  // so neither is here — the panel that inherits its configuration keeps it,
  // and what is actually wrong with the picture is being looked for elsewhere.
  cmd(SLPOUT);
  delay(120);
  const uint8_t fmt16 = 0x55;             // RGB565, the panel's native 16 bits
  cmd(COLMOD, &fmt16, 1);
  const uint8_t portrait = 0x00;          // row/column order as the layout assumes
  cmd(MADCTL, &portrait, 1);
  // ST7789 glass on these modules is fitted inverted; without this white is
  // black and the "dark" panel glows.
  cmd(DISPLAY_INVERT ? INVON : INVOFF);
  cmd(NORON);
  cmd(DISPON);

  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();

  // Ask the controller what it is, on boards that wired its MISO back. A read
  // wants a slower clock than a write and one dummy byte before the three ID
  // bytes; a panel that cannot answer returns all-ones or all-zeroes, which is
  // the expected reading on a three-wire panel rather than a fault.
#if PIN_TFT_MISO >= 0
  {
    _spi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_TFT_CS, LOW);
    digitalWrite(PIN_TFT_DC, LOW);
    _spi->write(0x04);                    // RDDID
    digitalWrite(PIN_TFT_DC, HIGH);
    _spi->transfer(0x00);                 // dummy clock the controller needs
    const uint8_t a = _spi->transfer(0x00);
    const uint8_t b = _spi->transfer(0x00);
    const uint8_t c = _spi->transfer(0x00);
    digitalWrite(PIN_TFT_CS, HIGH);
    _spi->endTransaction();
    _id = ((uint32_t)a << 16) | ((uint32_t)b << 8) | c;
  }
#endif

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

  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
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
    _spi->writeBytes(line, sizeof(line));            // the row, doubled across...
    _spi->writeBytes(line, sizeof(line));            // ...and down
  }
  memcpy(_shadow + (size_t)y0 * stride, fb + (size_t)y0 * stride,
         (size_t)(y1 - y0 + 1) * stride);

  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();

  // The first frame is on the glass; only now is the backlight worth its
  // current. Before this the panel shows the controller's power-on noise.
  if (!_lit) { _lit = true; applyBacklight(); }
}

void TftPanel::blank(bool on) {
  if (!_ok) return;
  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(on ? DISPOFF : DISPON);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();
  _blanked = on;
  // _lit before the relight, not after: applyBacklight() refuses to light a
  // panel that says it is unlit, and the old order left the PWM at zero on
  // every wake from a full blank — a black glass only a reboot recovered.
  _lit = !on;
  if (on) backlightSet(0); else applyBacklight();
}

#endif // HAS_DISPLAY && TFT
