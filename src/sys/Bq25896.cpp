// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Bq25896.cpp — see Bq25896.h. Register numbers from the TI datasheet.
// ============================================================================
#include "Bq25896.h"

#if HAS_BQ25896

#include <Arduino.h>
#include <Wire.h>
#include "I2cReg.h"

namespace {
constexpr uint8_t kAddr    = 0x6B;
constexpr uint8_t kReg09   = 0x09;       // BATFET_DIS lives here, bit 5
constexpr uint8_t kReg0B   = 0x0B;       // VBUS_STAT / CHRG_STAT
constexpr uint8_t kReg14   = 0x14;       // part number + revision

bool sPresent = false;

int  readReg(uint8_t reg)               { return I2cReg::read(Wire, kAddr, reg); }
bool writeReg(uint8_t reg, uint8_t val) { return I2cReg::write(Wire, kAddr, reg, val); }
} // namespace

namespace Bq25896 {

void begin() {
  // Checked, because 0x6B is a popular address and ship mode pointed at the
  // wrong chip is a brick. The part number field reads 0b000 on a real
  // BQ25896 — the family value it shares with the BQ25890/92; revision 2 is
  // what sets the '96 apart, and the kernel's driver keys on the same pair.
  // The first draft demanded 0b100 and rejected the genuine part.
  const int id = readReg(kReg14);
  if (id >= 0 && ((id >> 3) & 0x07) == 0b000) {
    sPresent = true;
    log_i("charger: BQ25896 answers at 0x6B (rev %d)", id & 0x03);
    return;
  }
  if (id >= 0) log_w("charger: 0x6B answers but is not a BQ25896 (reg14=0x%02x)", id);
}

bool present() { return sPresent; }

bool charging() {
  if (!sPresent) return false;
  // Cached on the gauge's own cadence: the status bar asks every second, and
  // a fresh bus transaction per ask bought nothing for a state that changes
  // over minutes. A failed read keeps the previous answer — "not charging"
  // invented from a bus hiccup is the confident wrong answer Power.h warns
  // against.
  static bool     sCharging = false;
  static uint32_t sReadMs   = 0;
  static bool     sEverRead = false;
  const uint32_t now = millis();
  if (!sEverRead || now - sReadMs >= BATTERY_SAMPLE_MS) {
    const int s = readReg(kReg0B);
    if (s >= 0) {
      const uint8_t chrg = (s >> 3) & 0x03;  // 00 none, 01 pre, 10 fast, 11 done
      sCharging = chrg == 1 || chrg == 2;
      sEverRead = true;
    }
    sReadMs = now;
  }
  return sCharging;
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
