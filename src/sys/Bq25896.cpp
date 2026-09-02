// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Bq25896.cpp — see Bq25896.h. Register numbers from the TI datasheet.
// ============================================================================
#include "Bq25896.h"

#if HAS_BQ25896

#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t kAddr    = 0x6B;
constexpr uint8_t kReg09   = 0x09;       // BATFET_DIS lives here, bit 5
constexpr uint8_t kReg0B   = 0x0B;       // VBUS_STAT / CHRG_STAT
constexpr uint8_t kReg14   = 0x14;       // part number + revision

bool sPresent = false;

int readReg(uint8_t reg) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(kAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
} // namespace

namespace Bq25896 {

void begin() {
  // The caller owns Wire.begin(); this only asks who answers. The part
  // number field is 0b100 for the BQ25896 — checked, because 0x6B is a
  // popular address and ship mode pointed at the wrong chip is a brick.
  const int id = readReg(kReg14);
  sPresent = id >= 0 && ((id >> 3) & 0x07) == 0b100;
  if (sPresent) log_i("charger: BQ25896 answers at 0x6B (rev %d)", id & 0x03);
  else if (id >= 0) log_w("charger: 0x6B answers but is not a BQ25896 (reg14=0x%02x)", id);
}

bool present() { return sPresent; }

bool charging() {
  if (!sPresent) return false;
  const int s = readReg(kReg0B);
  if (s < 0) return false;
  const uint8_t chrg = (s >> 3) & 0x03;  // 00 none, 01 pre, 10 fast, 11 done
  return chrg == 1 || chrg == 2;
}

bool vbusPowered() {
  if (!sPresent) return false;
  const int s = readReg(kReg0B);
  return s >= 0 && ((s >> 5) & 0x07) != 0;
}

void shipMode() {
  if (!sPresent) return;
  log_w("charger: entering ship mode — the power button brings the node back");
  delay(50);                             // let the line above leave the UART
  int r = readReg(kReg09);
  if (r < 0) r = 0x40;                   // datasheet default, if the read failed
  writeReg(kReg09, (uint8_t)(r | 0x20)); // BATFET_DIS
  delay(1000);                           // the FET opens within tens of ms
  log_e("charger: still alive after ship mode — VBUS is probably holding us up");
}

} // namespace Bq25896
#endif // HAS_BQ25896
