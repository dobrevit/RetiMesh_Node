// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  I2cReg.h — a register behind an I2C address, said once
//
//  The charger and the accelerometer each carried their own copy of these
//  three transactions; a bus-handling fix would have had six homes. The
//  repeated start on reads is deliberate: both parts NAK a bare read.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "Config.h"

namespace I2cReg {

// Which hosts are up, and on whose pins. Held inside functions rather than as
// plain variables because this is a header: a static at namespace scope would
// be one copy per translation unit, and one copy per unit is precisely the bug
// this file exists to prevent — each unit would think it had started the bus
// and none would agree about the pins.
namespace detail {
inline bool& mainStarted() { static bool v = false; return v; }
inline bool& h1Started()   { static bool v = false; return v; }
inline int&  h1Sda()       { static int  v = -1;    return v; }
inline int&  h1Scl()       { static int  v = -1;    return v; }
} // namespace detail

// The board's general-purpose I2C, started once however many drivers want it.
//
// It used to be started in setup(), inside the guard for the two parts that
// were then its only residents — so a board with neither never started it at
// all. That was correct while every other I2C device had a bus of its own, and
// stopped being correct with the first board that puts its touch controller
// and its keyboard on this one: two drivers, neither of which owns the bus,
// both of which need it up, and each initialised at a different point in
// setup(). Whoever asks first brings it up; everyone else gets the same bus.
inline TwoWire& mainBus() {
  if (!detail::mainStarted()) {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
    detail::mainStarted() = true;
  }
  return Wire;
}

// The bus a device's own pins put it on, started once. Where they are the
// board's general pair it is that bus; anything else is I2C host 1, brought up
// on the first caller's pins.
//
// One place decides, because two drivers deciding separately both reached for
// Wire1 — and the second one's begin() would have re-initialised the bus under
// the first, moving a working device onto another device's pins and leaving it
// reading as a wiring fault. That is the I2C shape of what SpiBus.h exists to
// stop, and it is a rule about the board rather than about any one driver.
//
// `up` reports whether the bus is usable, which is the answer a driver needs
// before it decides its part is missing.
inline TwoWire& busFor(int sda, int scl, uint32_t hz, bool* up = nullptr) {
  if (sda == PIN_I2C_SDA && scl == PIN_I2C_SCL) {
    if (up) *up = true;
    return mainBus();
  }
  if (!detail::h1Started()) {
    detail::h1Started() = Wire1.begin(sda, scl, hz);
    detail::h1Sda() = sda; detail::h1Scl() = scl;
  } else if (detail::h1Sda() != sda || detail::h1Scl() != scl) {
    log_e("I2C host 1 was started on SDA %d, SCL %d and is now asked for %d, %d "
          "— check the board header; the first wiring stands",
          detail::h1Sda(), detail::h1Scl(), sda, scl);
  }
  if (up) *up = detail::h1Started();
  return Wire1;
}

// Every address that answers, written into out as " 0x12 0x34" and returned as
// a count. A zero-length write is the probe: it addresses the part and sends no
// register, so one that is there acknowledges and one that is not does not.
//
// This lived in the touch driver, which meant a board without a touch
// controller never enumerated its bus at all — and the board in this fleet with
// the most parts on I2C is exactly the one with no touch layer. The count is
// returned separately from the text because the text can run out of room and
// the number of parts on a bus is the answer that matters first.
// The range is 0x08 to 0x7f, not the 0x08–0x77 every scanner writes. Both ends
// of the seven-bit space are reserved — 0x00–0x07 for broadcasts and the like,
// 0x78–0x7f for ten-bit addressing — and the convention is to probe neither.
// The convention is wrong about the top end: QST ship the QMC6309 magnetometer
// at 0x7c, inside the reserved block, and this board has one. Scanned at
// 0x08–0x77 that part is invisible, which reads as a magnetometer that is not
// fitted — and the wiring, the pins and the bus are all fine. The low end is
// still left alone, where a general-call probe can make unrelated parts answer.
inline size_t scan(TwoWire& bus, char* out, size_t cap) {
  size_t n = 0, found = 0;
  if (cap) out[0] = '\0';
  for (uint8_t a = 0x08; a <= 0x7f; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() != 0) continue;
    found++;
    if (n + 6 < cap) n += snprintf(out + n, cap - n, " 0x%02x", a);
  }
  return found;
}

// A bus this board has open, for a caller that wants to report the wiring
// rather than talk on it.
struct Host {
  TwoWire* bus;
  int      sda;
  int      scl;
  bool     shared;                       // the board's general pair, or a private one
};

// Both hosts, as far as they exist: the board's general pair always, and host 1
// only if some driver has claimed it. Returns how many were filled.
//
// Asking for the general pair starts it, which is deliberate. A board can carry
// parts on that bus that no driver here reaches yet — the M9's clock, compass
// and IMU are all sitting on it — and on such a board nothing would ever have
// opened it, so a reporter that only listed started buses would report the one
// bus that has nothing on it and stay silent about the one that does.
inline size_t hosts(Host* out, size_t cap) {
  size_t n = 0;
  // A board that names no general pair is a board with no such bus. Worth the
  // check because this is now called from the console rather than from a driver
  // that knew its own board: reporting must not be the thing that starts a bus
  // on pin -1.
  if (PIN_I2C_SDA >= 0 && PIN_I2C_SCL >= 0 && n < cap)
    out[n++] = Host{&mainBus(), PIN_I2C_SDA, PIN_I2C_SCL, true};
  if (detail::h1Started() && n < cap)
    out[n++] = Host{&Wire1, detail::h1Sda(), detail::h1Scl(), false};
  return n;
}

inline int read(TwoWire& bus, uint8_t addr, uint8_t reg) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return -1;
  if (bus.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return bus.read();
}

inline bool readN(TwoWire& bus, uint8_t addr, uint8_t reg, uint8_t* out, size_t n) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom(addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) out[i] = bus.read();
  return true;
}

inline bool write(TwoWire& bus, uint8_t addr, uint8_t reg, uint8_t val) {
  bus.beginTransmission(addr);
  bus.write(reg);
  bus.write(val);
  return bus.endTransmission() == 0;
}

} // namespace I2cReg
