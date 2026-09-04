// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Rtc.cpp — see Rtc.h
//
//  The PCF8563 keeps the time in eight registers of packed BCD, starting at
//  0x02. Two details in that block decide whether a reading means anything:
//
//  The top bit of the seconds register is VL — voltage low. The part sets it
//  when its oscillator has stopped, and it stays set until something writes the
//  seconds register again. So VL is not "the battery is flat now"; it is "the
//  time in here has been wrong at some point since anyone last set it", which
//  is the question actually worth asking. A reading taken with VL set is the
//  number the counter happened to reach, not a time.
//
//  The top bit of the months register is the century. The part has no notion of
//  which century it is in — the bit simply toggles when the years roll 99 to
//  00 — so it means whatever the firmware that wrote it meant. This one writes
//  it clear and reads years as 20xx, which is a decision that outlives none of
//  us and is worth saying out loud rather than leaving as an unexplained mask.
// ============================================================================
#include "Rtc.h"

#if HAS_RTC

#include <Arduino.h>
#include <sys/time.h>
#include "I2cReg.h"

namespace {

// The block of eight, from the seconds up.
constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegControl2 = 0x01;
constexpr uint8_t kRegSeconds  = 0x02;   // bit 7 is VL
constexpr uint8_t kVoltageLow  = 0x80;
constexpr uint8_t kStop        = 0x20;   // control 1: the counter is halted

// Anything before this is not a time this node was ever told. The same figure
// RnsTransport judges a telemetry stamp by, and for the same reason: below it,
// a "date" is a counter that started at the epoch wearing one.
constexpr time_t kClockIsRealAfter = 1735689600;  // 2025-01-01 UTC

bool sUp   = false;
bool sLost = false;

inline TwoWire& bus() { return I2cReg::busFor(PIN_RTC_SDA, PIN_RTC_SCL, I2C_HZ); }

uint8_t fromBcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t toBcd(uint8_t v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

} // namespace

namespace Rtc {

bool present() { return sUp; }
bool lostPower() { return sLost; }

bool read(time_t& out) {
  if (!sUp) return false;
  uint8_t d[7];
  if (!I2cReg::readN(bus(), RTC_ADDR, kRegSeconds, d, sizeof(d))) return false;

  sLost = (d[0] & kVoltageLow) != 0;
  if (sLost) return false;                 // a number, but not a time

  struct tm t = {};
  t.tm_sec  = fromBcd(d[0] & 0x7F);
  t.tm_min  = fromBcd(d[1] & 0x7F);
  t.tm_hour = fromBcd(d[2] & 0x3F);
  t.tm_mday = fromBcd(d[3] & 0x3F);
  // d[4] is the weekday, which the part counts on its own and nothing here
  // needs: mktime works it out from the date and would disagree with a part
  // that had been set carelessly.
  t.tm_mon  = fromBcd(d[5] & 0x1F) - 1;
  t.tm_year = 2000 + fromBcd(d[6]) - 1900;  // the century bit, decided above

  // TZ is pinned to UTC by Gps::begin(), which makes this a UTC conversion —
  // the same assumption syncClock() makes about the receiver's time, and the
  // reason both can be compared without converting anything.
  const time_t epoch = mktime(&t);
  if (epoch < kClockIsRealAfter) return false;
  out = epoch;
  return true;
}

bool write(time_t t) {
  if (!sUp || t < kClockIsRealAfter) return false;
  struct tm g;
  if (!gmtime_r(&t, &g)) return false;

  // Written from the seconds up, in one transaction: the part increments while
  // it is being read or written, and a write split across two transactions can
  // land either side of a carry.
  uint8_t d[7];
  d[0] = toBcd((uint8_t)g.tm_sec);           // VL clear — this is a time now
  d[1] = toBcd((uint8_t)g.tm_min);
  d[2] = toBcd((uint8_t)g.tm_hour);
  d[3] = toBcd((uint8_t)g.tm_mday);
  d[4] = (uint8_t)g.tm_wday;
  d[5] = toBcd((uint8_t)(g.tm_mon + 1));     // century bit clear: 20xx
  d[6] = toBcd((uint8_t)((g.tm_year + 1900) % 100));

  bus().beginTransmission((uint8_t)RTC_ADDR);
  bus().write(kRegSeconds);
  for (size_t i = 0; i < sizeof(d); i++) bus().write(d[i]);
  if (bus().endTransmission() != 0) return false;
  sLost = false;

  // Said once, because a write that quietly does nothing is indistinguishable
  // from one that works on the board this was written on: the part here was
  // already keeping good time before any of this code ran, so "the clock is
  // right" proves nothing about the path that sets it. The board this matters
  // for is the one whose clock is flat, and on that board a silent failure
  // means the clock is never seeded and nobody finds out.
  static bool announced = false;
  if (!announced) {
    announced = true;
    log_i("rtc: seeded from the receiver, %04d-%02d-%02d %02d:%02d:%02d UTC",
          g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  }
  return true;
}

void begin() {
  bool up = false;
  I2cReg::busFor(PIN_RTC_SDA, PIN_RTC_SCL, I2C_HZ, &up);
  if (!up) { log_w("rtc: bus would not start (SDA %d, SCL %d)", PIN_RTC_SDA, PIN_RTC_SCL); return; }

  // The part answers whenever it is powered, so a plain probe is the whole
  // test — unlike a touch controller, it has no reason to stay silent.
  bus().beginTransmission((uint8_t)RTC_ADDR);
  if (bus().endTransmission() != 0) {
    log_w("rtc: nothing answered at 0x%02x on SDA %d, SCL %d — I2C on the console "
          "lists what did", (uint8_t)RTC_ADDR, PIN_RTC_SDA, PIN_RTC_SCL);
    return;
  }
  sUp = true;

  // Running, and with the alarm and timer interrupts it may have been left
  // holding switched off. A part that came up STOPped keeps perfect time in the
  // sense that it never changes, which is the worst way for this to fail.
  const int c1 = I2cReg::read(bus(), RTC_ADDR, kRegControl1);
  if (c1 >= 0 && (c1 & kStop)) {
    I2cReg::write(bus(), RTC_ADDR, kRegControl1, (uint8_t)(c1 & ~kStop));
    log_w("rtc: the counter was halted and has been started");
  }
  I2cReg::write(bus(), RTC_ADDR, kRegControl2, 0x00);

  time_t held = 0;
  if (read(held)) {
    // Before anything can timestamp. This is the whole purpose of the part: the
    // node is right from here rather than from whenever the sky clears.
    struct timeval tv = { .tv_sec = held, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    struct tm g;
    gmtime_r(&held, &g);
    log_i("rtc: clock set from the board's own, %04d-%02d-%02d %02d:%02d:%02d UTC",
          g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  } else {
    log_i("rtc: found at 0x%02x, holding no time worth having (%s) — the receiver "
          "will seed it", (uint8_t)RTC_ADDR,
          sLost ? "its oscillator stopped since it was last set" : "before 2025");
  }
}

} // namespace Rtc

#endif // HAS_RTC
