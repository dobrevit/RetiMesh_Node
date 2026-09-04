// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  TouchInput.cpp — see TouchInput.h
//
//  Two controllers, which are not alike enough to tell apart by probing:
//
//  The CHSC6X holds one report and answers I2C only while a finger is on the
//  glass. The report is five bytes — a point count, then two big-endian
//  twelve-bit coordinates with their high nibbles carrying flags to mask off.
//  No registers to set up: reset it, wait, and read when curious. Its silence
//  when idle is normal, which is exactly why it cannot be probed for.
//
//  The GT911 is the opposite: sixteen-bit register addresses, always answering,
//  and a status byte that says whether the coordinates behind it are fresh. It
//  will not report again until that byte is cleared, so a reader that forgets
//  to clear it sees one touch and then a dead panel forever.
//
//  Which one a board carries is TOUCH_KIND, from its header. Nothing here
//  guesses.
//
//  Both are polled rather than driven from an interrupt. On one board the
//  published map has the line commented out; on the other two sources disagree
//  about what the pin even is. A poll a few times a second costs one short
//  transaction and cannot be wrong about a wire.
// ============================================================================
#include "TouchInput.h"

#if HAS_TOUCH

#include <Arduino.h>
#include <Wire.h>
#include "I2cReg.h"

namespace {

// Which bus the controller is on. Where its pins are the board's general
// I2C — a board that puts the touch layer and the keyboard on one pair — it
// is that bus, brought up by whichever driver reaches it first. Where they
// differ the layer has a bus of its own, away from everything else, and this
// file owns it.
#if (PIN_TOUCH_SDA == PIN_I2C_SDA) && (PIN_TOUCH_SCL == PIN_I2C_SCL)
  #define TOUCH_ON_MAIN_I2C 1
#else
  #define TOUCH_ON_MAIN_I2C 0
  TwoWire sOwnBus(1);
#endif

inline TwoWire& bus() {
#if TOUCH_ON_MAIN_I2C
  return I2cReg::mainBus();
#else
  return sOwnBus;
#endif
}

bool    sUp   = false;
uint8_t sAddr = TOUCH_ADDR;               // the GT911 answers at one of two

#if TOUCH_KIND == TOUCH_KIND_GT911
// Sixteen-bit register addresses, big-endian on the wire.
constexpr uint16_t kRegStatus  = 0x814E;  // bit 7 = report ready, bits 3:0 = points
constexpr uint16_t kRegPoint1  = 0x8150;  // id, x lo/hi, y lo/hi, size lo/hi
constexpr uint16_t kRegProduct = 0x8140;  // four ASCII bytes, "911" on this part
constexpr uint8_t  kAddrAlt    = 0x14;    // the other strap

bool gt911Read(uint16_t reg, uint8_t* out, size_t n) {
  bus().beginTransmission(sAddr);
  bus().write((uint8_t)(reg >> 8));
  bus().write((uint8_t)(reg & 0xFF));
  if (bus().endTransmission(false) != 0) return false;
  if (bus().requestFrom(sAddr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) out[i] = bus().read();
  return true;
}

bool gt911Write(uint16_t reg, uint8_t val) {
  bus().beginTransmission(sAddr);
  bus().write((uint8_t)(reg >> 8));
  bus().write((uint8_t)(reg & 0xFF));
  bus().write(val);
  return bus().endTransmission() == 0;
}
#endif

} // namespace

namespace TouchInput {

void begin() {
#if defined(PIN_TOUCH_RST) && PIN_TOUCH_RST >= 0
  // The reference driver's order: reset released first and given time, then a
  // deliberate reset pulse once the part has settled. Boards that tie the
  // controller's reset to the board's own have nothing to pulse.
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(60);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(10);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(30);
#endif

#if TOUCH_ON_MAIN_I2C
  bus();                                  // shared: up already, or up now
  sUp = true;
#else
  sUp = sOwnBus.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, I2C_HZ);
  if (!sUp) { log_w("touch: controller bus would not start (SDA %d, SCL %d)",
                    PIN_TOUCH_SDA, PIN_TOUCH_SCL); return; }
#endif

  // Who is actually on this bus. On the CHSC6X the controller is expected to
  // NACK while nothing touches the glass, so its own silence proves little —
  // but a scan that finds a part at some *other* address, or a bus with
  // nothing on it at all, is exactly the wiring answer a dead layer needs.
  char found[48] = ""; size_t n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    bus().beginTransmission(a);
    if (bus().endTransmission() == 0 && n < sizeof(found) - 6)
      n += snprintf(found + n, sizeof(found) - n, " 0x%02x", a);
  }

#if TOUCH_KIND == TOUCH_KIND_GT911
  // This one does answer when idle, so it can be asked who it is. The strap on
  // the reset line picks between two addresses and the boards that carry it do
  // not all strap it the same way, so both are tried and the product ID — four
  // ASCII bytes, "911" on this part — settles it.
  uint8_t id[4] = {0};
  bool found911 = false;
  for (uint8_t attempt = 0; attempt < 2 && !found911; attempt++) {
    sAddr = attempt == 0 ? (uint8_t)TOUCH_ADDR : kAddrAlt;
    if (gt911Read(kRegProduct, id, sizeof(id)) && id[0] == '9' && id[1] == '1' && id[2] == '1')
      found911 = true;
  }
  if (!found911) {
    sAddr = TOUCH_ADDR;
    log_w("touch: no GT911 answered at 0x%02x or 0x%02x; acked now:%s",
          (uint8_t)TOUCH_ADDR, kAddrAlt, n ? found : " nothing");
    sUp = false;
    return;
  }
  gt911Write(kRegStatus, 0);              // start from a cleared report
  log_i("touch: GT911 \"%c%c%c%c\" at 0x%02x (SDA %d, SCL %d);%s%s",
        id[0], id[1], id[2], id[3] ? id[3] : ' ', sAddr, PIN_TOUCH_SDA, PIN_TOUCH_SCL,
        TOUCH_ON_MAIN_I2C ? " shared bus, acked:" : " own bus, acked:", n ? found : " nothing");
#else
  log_i("touch: bus up (SDA %d, SCL %d); acked now:%s (0x%02x expected; idle silence is normal)",
        PIN_TOUCH_SDA, PIN_TOUCH_SCL, n ? found : " nothing", TOUCH_ADDR);
#endif
}

Point poll() {
  Point p;
  if (!sUp) return p;

#if TOUCH_KIND == TOUCH_KIND_GT911
  uint8_t st = 0;
  if (!gt911Read(kRegStatus, &st, 1)) return p;
  // Bit 7 says the coordinates behind this byte are a fresh report. Without
  // it they are whatever the last touch left, and reporting those would hold
  // a finger down forever.
  if (!(st & 0x80)) return p;
  const uint8_t points = st & 0x0F;
  uint8_t d[6];
  if (points >= 1 && gt911Read(kRegPoint1, d, sizeof(d))) {
    // Little-endian here, unlike the register addresses that reach them.
    p.down = true;
    p.x = (int16_t)(d[1] | (d[2] << 8));
    p.y = (int16_t)(d[3] | (d[4] << 8));
  }
  // Cleared whether or not a point was read: the part reports nothing further
  // until this byte is zero, so an early return that skipped it would take the
  // layer down for good.
  gt911Write(kRegStatus, 0);
  return p;
#else
  // The controller is register-addressed: write the report pointer, then read
  // with a repeated start — a bare read gets nothing, which is why the first
  // version of this driver saw a dead layer under a working finger.
  bus().beginTransmission((uint8_t)TOUCH_ADDR);
  bus().write((uint8_t)0x00);
  if (bus().endTransmission(false) != 0 ||
      bus().requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)8) != 8) return p;
  uint8_t r[8];
  for (uint8_t i = 0; i < 8; i++) r[i] = bus().read();
  // The report's shape, from the reference driver: point count in byte 2,
  // first point in bytes 3-6, two poison patterns meaning "no report", and a
  // 0xC0 flag on byte 3 meaning the finger has lifted.
  const bool poison = (r[2] == 0 && r[3] == 0 && r[4] == 0 && r[6] == 0) ||
                      (r[2] == 0xFF && r[3] == 0xFF && r[4] == 0xFF && r[6] == 0xFF);
  if (poison || (r[2] & 0x07) == 0 || (r[3] & 0xC0) == 0xC0) return p;
  p.down = true;
  p.x = (int16_t)(((r[3] & 0x0F) << 8) | r[4]);
  p.y = (int16_t)(((r[5] & 0x0F) << 8) | r[6]);
  return p;
#endif
}

} // namespace TouchInput

#endif // HAS_TOUCH
