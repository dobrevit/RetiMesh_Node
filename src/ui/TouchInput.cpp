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
#endif

inline TwoWire& bus() {
#if TOUCH_ON_MAIN_I2C
  return I2cReg::mainBus();
#else
  // The core's own object for I2C 1 rather than a second TwoWire built on the
  // same peripheral. Nothing here shares that host today, but the keyboard
  // driver reaches for it on the boards that give it a pair of its own, and two
  // objects calling begin() on one host is exactly what SpiBus.h exists to stop
  // happening on SPI — the second one re-initialises the bus under the first.
  return Wire1;
#endif
}

bool    sUp   = false;
uint8_t sAddr = TOUCH_ADDR;               // the GT911 answers at one of two

// What the controller has actually reported since boot, and where it last put
// a finger. A layer that is found but never reports is a different fault from
// one that reports coordinates the shell then maps somewhere nobody pressed,
// and from the outside — a screen that ignores taps — the two look identical.
// Counted here so the console can tell them apart.
uint32_t sReports = 0;
int16_t  sLastX = -1, sLastY = -1;
// The report exactly as the controller sent it. Decoding it is where this
// driver was wrong — the coordinates came back far outside a panel this size,
// which is a byte-layout fault and not a rotation one — and the bytes are the
// only thing that settles which field is where.
uint8_t  sLastRaw[6] = {0};

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
  sUp = bus().begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, I2C_HZ);
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
  // Both addresses, several times over. Once was enough on a board whose touch
  // controller had a bus to itself; here it shares one with a keyboard that is
  // a microcontroller of its own, and a part that is still coming up answers
  // nothing at either address. A layer given up on at boot stays given up on
  // for the life of the run, so it is worth a few hundred milliseconds to be
  // sure — and the loop costs nothing once the part is there.
  for (uint8_t round = 0; round < 6 && !found911; round++) {
    if (round) delay(50);
    for (uint8_t attempt = 0; attempt < 2 && !found911; attempt++) {
      sAddr = attempt == 0 ? (uint8_t)TOUCH_ADDR : kAddrAlt;
      if (gt911Read(kRegProduct, id, sizeof(id)) && id[0] == '9' && id[1] == '1' && id[2] == '1')
        found911 = true;
    }
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

bool present() { return sUp; }

uint32_t reports() { return sReports; }
const uint8_t* lastRaw() { return sLastRaw; }
int16_t lastX() { return sLastX; }
int16_t lastY() { return sLastY; }

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
    // Little-endian here, unlike the register addresses that reach them — and
    // two bytes further in than the datasheet's register list suggests. The
    // read comes back one byte ahead of where the track id is expected, so the
    // coordinates sit at [2..3] and [4..5] rather than [1..2] and [3..4].
    //
    // Measured, not reasoned: decoded the other way a tap at the top-left
    // corner reported 2816,1792 — numbers with no meaning on a panel this
    // size — and the bytes behind them were 00 0b 00 07, which is 11 and 7.
    p.down = true;
    p.x = (int16_t)(d[2] | (d[3] << 8));
    p.y = (int16_t)(d[4] | (d[5] << 8));
    sReports++;
    sLastX = p.x; sLastY = p.y;
    memcpy(sLastRaw, d, sizeof(sLastRaw));
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
