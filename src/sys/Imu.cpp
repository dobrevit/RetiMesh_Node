// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Imu.cpp — see Imu.h
//
//  The DA217 numbers come from the MiraMEMS datasheet. The vendor's spec sheet
//  names the part; an earlier draft guessed a QMI8658 off the factory image's
//  multi-driver probe strings, which is the lesson: a probe list names what the
//  firmware could drive, not what the board has.
//
//  The QMI8658 numbers come from QST's own datasheet (rev 0.9), and this time
//  the part was read off the bus before a line was written — WHO_AM_I 0x05 at
//  0x6b on the M9, by `I2C 0x6b` on the console. Which also caught the trap
//  below: this part does not auto-increment its register pointer by default, so
//  a burst read of the output block returns the same byte six times. Written
//  from the register map alone, that reads as a dead part on a good bus.
// ============================================================================
#include "Imu.h"

#if HAS_IMU

#include <Arduino.h>
#include <Wire.h>
#include "I2cReg.h"
#include "Lock.h"

namespace {

uint8_t sAddr = 0;                       // 0 = nothing answered

// What the part is doing, and what it has been asked to do. Unlike the
// magnetometer this part has readers on two different tasks across the fleet —
// the display task asks which way up the panel is held, the main loop asks
// where level went for the compass's tilt correction — so the mode write is
// not enough on its own: every register access here is taken under one lock,
// so a read and a mode change on different cores cannot interleave inside the
// Arduino bus object's own begin/end sequence.
SemaphoreHandle_t sLock = nullptr;
volatile bool     sWantRunning = true;   // raised from any task
bool              sRunning     = false;  // written under the lock

inline TwoWire& bus() { return I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ); }

int  readReg(uint8_t reg)                          { return I2cReg::read(bus(), sAddr, reg); }
bool readRegs(uint8_t reg, uint8_t* out, size_t n) { return I2cReg::readN(bus(), sAddr, reg, out, n); }
bool writeReg(uint8_t reg, uint8_t val)            { return I2cReg::write(bus(), sAddr, reg, val); }

#if IMU_KIND == IMU_KIND_QMI8658
// QST QMI8658, datasheet rev 0.9.
constexpr uint8_t kWhoAmI   = 0x00;      // 0x05 on every part in this family
constexpr uint8_t kCtrl1    = 0x02;      // bit 6 ADDR_AI, bit 5 BE
constexpr uint8_t kCtrl2    = 0x03;      // bits 6:4 full scale, bits 3:0 rate
constexpr uint8_t kCtrl7    = 0x08;      // bit 0 accelerometer on
// The one bit that starts and stops the accelerometer, named at both values
// because both are now written: clearing it is the part's own low-power state,
// and it is the same bit begin() sets, not a mode read off a datasheet nothing
// here has checked.
constexpr uint8_t kCtrl7On  = 0x01;
constexpr uint8_t kCtrl7Off = 0x00;
constexpr uint8_t kAccelX   = 0x35;      // AX_L; six bytes to AZ_H
// ±2 g at 62.5 Hz. The smallest range because this is asked about gravity and
// nothing faster, and the smallest range is the finest resolution: 16384 counts
// per g, from the datasheet's sensitivity table.
constexpr uint8_t kCtrl2Value = 0x07;
constexpr float   kCountsPerG = 16384.0f;
#else
// MiraMEMS DA217.
constexpr uint8_t kChipId     = 0x01;    // 0x13
constexpr uint8_t kAccelX     = 0x02;
// Power mode. Both values come from the same place as the enable value this
// driver already used — the kernel's da280 driver, which names 0x1e and 0x9e
// as the pair — so the suspend half is as documented as the run half, and the
// difference between them is the one bit the family reserves for it.
constexpr uint8_t kMode        = 0x11;
constexpr uint8_t kModeNormal  = 0x1e;
constexpr uint8_t kModeSuspend = 0x9e;
// 14-bit, left-justified, at the ±2 g the part is put in below. Unverified on
// hardware: the only board carrying this part has no magnetometer, so nothing
// asks it for a number rather than a ratio. facing() proves the axes; this
// proves nothing until a board needs it.
constexpr float   kCountsPerG = 4096.0f;
#endif

// The three axes as signed counts, however this part packs them. False when the
// read failed — which on the QMI8658 includes the case that matters, since a
// part whose auto-increment was never enabled returns one register six times.
bool rawAxes(int16_t& x, int16_t& y, int16_t& z) {
  if (!sAddr) return false;
  Sys::Lock held(sLock);
  // A suspended part still answers its address and still returns the last
  // conversion it made, which is the same trap begin() guards against: the
  // reading would look like a board that has not moved since the screen went
  // dark. Nothing is the honest answer, and both callers already have one.
  if (!sRunning) return false;
  uint8_t raw[6];
  if (!readRegs(kAccelX, raw, sizeof(raw))) return false;
#if IMU_KIND == IMU_KIND_QMI8658
  x = (int16_t)(raw[0] | (raw[1] << 8));
  y = (int16_t)(raw[2] | (raw[3] << 8));
  z = (int16_t)(raw[4] | (raw[5] << 8));
#else
  // Left-justified little-endian pairs; the shift keeps the sign.
  x = (int16_t)((int16_t)(raw[0] | (raw[1] << 8)) >> 4);
  y = (int16_t)((int16_t)(raw[2] | (raw[3] << 8)) >> 4);
  z = (int16_t)((int16_t)(raw[4] | (raw[5] << 8)) >> 4);
#endif
  // Into the panel's frame, which is the only frame the callers think in. Both
  // of them — the display's rotation and the compass's tilt correction — ask
  // "which way is down relative to the screen", so the mounting has to be
  // undone here rather than in each of them.
#if IMU_INVERT_X
  x = (int16_t)-x;
#endif
#if IMU_INVERT_Y
  y = (int16_t)-y;
#endif
#if IMU_INVERT_Z
  z = (int16_t)-z;
#endif
  return true;
}

} // namespace

namespace Imu {

void begin() {
  // Before the first register access, and before any task can ask for one.
  // A null handle is not fatal — Sys::Lock treats it as no lock — so a board
  // that could not allocate one behaves exactly as this driver did before.
  if (!sLock) sLock = xSemaphoreCreateMutex();
#if IMU_KIND == IMU_KIND_QMI8658
  // One address, strapped by SDO and not by anything this board can change.
  sAddr = IMU_ADDR;
  if (readReg(kWhoAmI) != 0x05) {
    log_i("imu: no QMI8658 answers at 0x%02x — I2C on the console lists what does",
          (uint8_t)IMU_ADDR);
    sAddr = 0;
    return;
  }
  // Auto-increment first, and it is not optional: without it every register in
  // a burst read comes back as the first one, so the output block reads as six
  // copies of the X low byte. BE left clear, so a pair is low byte then high.
  //
  // Checked, all three. This part answers its address whatever state it is in,
  // so a configuration that did not land leaves an accelerometer that reads —
  // the same six bytes for ever, because auto-increment is the very write most
  // worth losing. Six copies of one byte is what an unconfigured part looks
  // like, and it is exactly what the bus scan saw before any of this existed.
  bool configured = writeReg(kCtrl1, 0x40);
  configured = configured && writeReg(kCtrl2, kCtrl2Value);
  configured = configured && writeReg(kCtrl7, kCtrl7On);  // accelerometer only; the gyro
                                                         // costs milliamps and answers
                                                         // nothing asked here
  if (!configured) {
    log_w("imu: QMI8658 answered at 0x%02x but would not take its configuration "
          "— left off rather than reading the same six bytes for ever", sAddr);
    sAddr = 0;
    return;
  }
  sRunning = true;
  log_i("imu: QMI8658 at 0x%02x, accelerometer running at +/-2 g", sAddr);
#else
  for (uint8_t addr : { (uint8_t)0x26, (uint8_t)0x27 }) {
    sAddr = addr;
    if (readReg(kChipId) == 0x13) break; // chip id says DA217
    sAddr = 0;
  }
  if (!sAddr) { log_i("imu: no DA217 answers"); return; }
  // 0x1e is the documented enable value for this family (the kernel's da280
  // driver uses exactly it); the first draft wrote 0x00, whose bandwidth code
  // the datasheet reserves — it ran, but on the datasheet's silence.
  writeReg(kMode, kModeNormal);          // normal power mode, documented bandwidth
  writeReg(0x0F, 0x00);                  // ±2g — orientation needs no more
  sRunning = true;
  log_i("imu: DA217 at 0x%02x, accelerometer running", sAddr);
#endif
}

bool present() { return sAddr != 0; }

bool running() { return sRunning; }

void setRunning(bool run) { sWantRunning = run; }

void poll() {
  // Read without the lock on purpose: two aligned flags with one writer each
  // and no companion state, so the worst a torn view can do is defer the
  // change to the next pass a millisecond later. Taking a mutex a thousand
  // times a second to discover there is nothing to do is the cost this guard
  // exists to avoid.
  if (!sAddr || sWantRunning == sRunning) return;
  Sys::Lock held(sLock);
  const bool want = sWantRunning;
  if (want == sRunning) return;          // settled while we waited for the lock
#if IMU_KIND == IMU_KIND_QMI8658
  if (!writeReg(kCtrl7, want ? kCtrl7On : kCtrl7Off)) return;
#else
  if (!writeReg(kMode, want ? kModeNormal : kModeSuspend)) return;
#endif
  // Only once the write landed. A part recorded as suspended that is still
  // converting costs power silently; one recorded as running that is not
  // reads as a board that never moves, and this driver already refuses to
  // ship that. A failed write leaves the want standing for the next pass.
  sRunning = want;
}

bool accel(float g[3]) {
  int16_t x, y, z;
  if (!rawAxes(x, y, z)) return false;
  g[0] = (float)x / kCountsPerG;
  g[1] = (float)y / kCountsPerG;
  g[2] = (float)z / kCountsPerG;
  return true;
}

Facing facing() {
  int16_t ax, ay, az;
  if (!rawAxes(ax, ay, az)) return Facing::Unknown;
  // Only the axes' ratios matter here, so the exact scale is deliberately not
  // chased — every axis is scaled alike whichever part answered.
  const int32_t mx = ax < 0 ? -ax : ax, my = ay < 0 ? -ay : ay, mz = az < 0 ? -az : az;
  // Lying flat, gravity is all Z and the panel's rotation is nobody's guess.
  if (mz > mx * 2 && mz > my * 2) return Facing::Flat;
  if (my >= mx) return ay < 0 ? Facing::Up0 : Facing::Up180;
  return ax < 0 ? Facing::Up90 : Facing::Up270;
}

} // namespace Imu

#endif // HAS_IMU
