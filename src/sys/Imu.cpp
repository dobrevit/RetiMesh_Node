// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Imu.cpp — see Imu.h. Register numbers from the MiraMEMS DA217 datasheet.
//  The vendor's spec sheet names the part; an earlier draft guessed a QMI8658
//  off the factory image's multi-driver probe strings, which is the lesson:
//  a probe list names what the firmware could drive, not what the board has.
// ============================================================================
#include "Imu.h"

#if HAS_DA217

#include <Arduino.h>
#include <Wire.h>

namespace {
uint8_t sAddr = 0;                       // 0x26 or 0x27, SDO-strapped; 0 = absent

int readReg(uint8_t reg) {
  Wire.beginTransmission(sAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(sAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool readRegs(uint8_t reg, uint8_t* out, size_t n) {
  Wire.beginTransmission(sAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(sAddr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(sAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
} // namespace

namespace Imu {

void begin() {
  for (uint8_t addr : { (uint8_t)0x26, (uint8_t)0x27 }) {
    sAddr = addr;
    if (readReg(0x01) == 0x13) break;    // chip id says DA217
    sAddr = 0;
  }
  if (!sAddr) { log_i("imu: no DA217 on the main bus"); return; }
  writeReg(0x11, 0x00);                  // normal power mode
  writeReg(0x0F, 0x00);                  // ±2g — orientation needs no more
  log_i("imu: DA217 at 0x%02x, accelerometer running", sAddr);
}

bool present() { return sAddr != 0; }

Facing facing() {
  if (!sAddr) return Facing::Unknown;
  uint8_t raw[6];
  if (!readRegs(0x02, raw, sizeof(raw))) return Facing::Unknown;
  // 12-bit left-justified little-endian pairs; the shift keeps the sign.
  const int16_t ax = (int16_t)(raw[0] | (raw[1] << 8)) >> 4;
  const int16_t ay = (int16_t)(raw[2] | (raw[3] << 8)) >> 4;
  const int16_t az = (int16_t)(raw[4] | (raw[5] << 8)) >> 4;
  const int32_t mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay, mz = az < 0 ? -az : az;
  // Lying flat, gravity is all Z and the panel's rotation is nobody's guess.
  if (mz > mx * 2 && mz > my * 2) return Facing::Flat;
  if (my >= mx) return ay < 0 ? Facing::Up0 : Facing::Up180;
  return ax < 0 ? Facing::Up90 : Facing::Up270;
}

} // namespace Imu
#endif // HAS_DA217
