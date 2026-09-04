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
  static bool started = false;
  if (!started) {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
    started = true;
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
  static bool started = false;
  static int  sSda = -1, sScl = -1;
  if (!started) {
    started = Wire1.begin(sda, scl, hz);
    sSda = sda; sScl = scl;
  } else if (sSda != sda || sScl != scl) {
    log_e("I2C host 1 was started on SDA %d, SCL %d and is now asked for %d, %d "
          "— check the board header; the first wiring stands", sSda, sScl, sda, scl);
  }
  if (up) *up = started;
  return Wire1;
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
