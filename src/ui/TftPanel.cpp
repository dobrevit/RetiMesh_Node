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
constexpr uint8_t SLPIN   = 0x10;
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
// The controller's settling time either side of sleep, from the datasheet:
// after SLPOUT it will not take another command until its booster and
// oscillator are up, and after SLPIN it will not take a SLPOUT until they
// have properly stopped. One number, because it is one specification — begin()
// waits it too, and the wake path below is a copy of what begin() does rather
// than a delay somebody guessed.
constexpr uint32_t SLEEP_SETTLE_MS = 120;
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
  // The percent-to-duty mapping (clamp included) is DisplayLayout's, shared
  // with the OLED's contrast register rather than written out again here.
  const uint32_t duty = DisplayLayout::brightnessLevel(pct);
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
  // Stored as given: both backlight kinds clamp inside their own mapping
  // (DisplayLayout::brightnessLevel for PWM, the sixteen-step clamp above
  // for the pulse-counted dimmer), so a second clamp here would be the
  // duplicate rule this file just gave up.
  _brightPct = pct;
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
  delay(SLEEP_SETTLE_MS);
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

// DISPOFF alone was never sleep. It stops the output while the booster, the
// oscillator and the frame-RAM refresh go on running — milliamps, on the one
// part that costs more than the radio while somebody is holding the board. So
// the controller is put to sleep behind it (SLPIN) and woken in front of it
// (SLPOUT), which is the pair the datasheet defines and the pair begin()
// already uses on the way up.
//
// Both settling waits are spent outside the SPI transaction. On the T-Deck the
// radio, the card and this panel are the same three wires (SpiBus.h); holding
// the bus for a tenth of a second would stall the radio task behind a screen
// going dark, which is a good deal worse than the current it saves.
void TftPanel::blank(bool on) {
  if (!_ok) return;
  // Idempotent, and now worth saying so: setBlank(true) is reached from the
  // idle timer, the power menu, a long press and the deep-sleep path, several
  // of which arrive with the panel already dark. Re-sending the pair would
  // pay a settling wait for a state the panel is already in. Both flags are
  // tested rather than one, so the boot state — unlit and unblanked, waiting
  // for the first frame — is not mistaken for a lit panel.
  if (_blanked == on && _lit != on) return;

  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  if (on) {
    cmd(DISPOFF);                         // output off...
    cmd(SLPIN);                           // ...and the parts behind it stopped
  } else {
    cmd(SLPOUT);                          // the wake's one command that waits
  }
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();

  _blanked = on;
  // _lit before the relight, not after: applyBacklight() refuses to light a
  // panel that says it is unlit, and the old order left the PWM at zero on
  // every wake from a full blank — a black glass only a reboot recovered.
  _lit = !on;
  // The LED goes with the command that blanked the glass, not after the wait
  // below: a tenth of a second of backlight over a panel that has already
  // stopped driving it is a dim grey rectangle, and it is the current this
  // whole function exists to stop.
  if (on) backlightSet(0);

  // One wait per edge, never one per command. Waking, it is the settle SLPOUT
  // owes before the controller will take the DISPON below. Blanking, it is the
  // same specification read the other way: the controller refuses a SLPOUT
  // that arrives too soon after a SLPIN, and a tap landing inside that window
  // — the button path wakes immediately, without the dark panel's 250 ms poll
  // gate — would otherwise be answered by a panel that stayed asleep and dark.
  // Spending it here rather than saving it for the wake is deliberate: nothing
  // is waiting on a screen that has just gone off, something is always waiting
  // on one coming back, and the wake is the half with a time budget on it.
  delay(SLEEP_SETTLE_MS);
  if (on) return;

  _spi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_TFT_CS, LOW);
  cmd(DISPON);
  digitalWrite(PIN_TFT_CS, HIGH);
  _spi->endTransaction();
  // And only now the backlight, over a frame the controller is driving again.
  // Nothing is done to the shadow, which is deliberate rather than forgotten:
  // the controller keeps its frame memory through sleep — only the booster,
  // the oscillator and the panel scan stop — so what flush() believes is on
  // the glass is still true and the band-diff stays worth having across a
  // blank. The shell does not use that buffer at all; it repaints from LVGL,
  // which invalidates the screen on this same edge.
  applyBacklight();
}

#endif // HAS_DISPLAY && TFT
