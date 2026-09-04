// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Compass.cpp — see Compass.h
//
//  The QMC6309's register map is from QST's part and from a reading of the part
//  itself: `I2C 0x7c` on the console answered chip id 0x90 at register 0, with
//  both control registers at zero — which is this part saying it is suspended
//  and nobody has configured it. That is the state begin() takes it out of.
//
//  Note the address. 0x7c is inside the block the I2C specification reserves
//  for ten-bit addressing, which is why the bus scan had to be widened past the
//  conventional 0x77 to see this part at all. QST put it there; the scan works
//  around it (I2cReg.h).
// ============================================================================
#include "Compass.h"

#if HAS_COMPASS

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "I2cReg.h"
#include "Imu.h"

namespace {

constexpr uint8_t kChipId   = 0x00;      // 0x90
constexpr uint8_t kDataX    = 0x01;      // six bytes, little-endian x, y, z
constexpr uint8_t kStatus   = 0x09;
constexpr uint8_t kCtrl1    = 0x0A;
constexpr uint8_t kCtrl2    = 0x0B;

// Continuous mode, the part's fastest oversampling, 200 Hz. The rate costs
// nothing here — this is read when something asks, not on a timer — and the
// oversampling is what makes a single reading steady enough to be worth taking.
constexpr uint8_t kCtrl1Run = 0xD3;
constexpr uint8_t kCtrl2Run = 0x03;
constexpr uint8_t kSoftReset = 0x80;

// Microtesla per count, from the part's scale at the range set above.
constexpr float kUtPerCount = 0.0488f;

bool sUp = false;

// The extremes each axis has reached, which is how the hard-iron offset is
// found: the readings from a board turned about lie on a sphere, and the centre
// of that sphere is the offset to subtract. Seeded inverted so the first
// reading replaces both ends.
float sMin[3] = {  1e9f,  1e9f,  1e9f };
float sMax[3] = { -1e9f, -1e9f, -1e9f };

inline TwoWire& bus() { return I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ); }

} // namespace

namespace Compass {

bool present() { return sUp; }

void begin() {
  bool up = false;
  I2cReg::busFor(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ, &up);
  if (!up) { log_w("compass: bus would not start"); return; }

  const int id = I2cReg::read(bus(), COMPASS_ADDR, kChipId);
  if (id != 0x90) {
    log_i("compass: no QMC6309 at 0x%02x (chip id read %d, wanted 0x90) — "
          "I2C on the console lists what answered", (uint8_t)COMPASS_ADDR, id);
    return;
  }

  // Reset before configuring. The part keeps running across a firmware restart
  // — nothing here powers it down — so without this it would be configured
  // from whatever state the previous run, or the board's factory firmware, left
  // it in rather than from a known one.
  I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, kSoftReset);
  delay(10);
  I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, 0x00);
  delay(10);
  I2cReg::write(bus(), COMPASS_ADDR, kCtrl2, kCtrl2Run);
  I2cReg::write(bus(), COMPASS_ADDR, kCtrl1, kCtrl1Run);
  delay(10);

  sUp = true;
  log_i("compass: QMC6309 at 0x%02x, continuous; heading needs the board turned "
        "around once before the hard-iron offsets mean anything", (uint8_t)COMPASS_ADDR);
}

Reading read() {
  Reading r;
  if (!sUp) return r;

  uint8_t d[6];
  if (!I2cReg::readN(bus(), COMPASS_ADDR, kDataX, d, sizeof(d))) return r;

  float raw[3];
  for (int i = 0; i < 3; i++) {
    const int16_t v = (int16_t)(d[i * 2] | (d[i * 2 + 1] << 8));
    raw[i] = (float)v * kUtPerCount;
    r.magUt[i] = raw[i];
    if (raw[i] < sMin[i]) sMin[i] = raw[i];
    if (raw[i] > sMax[i]) sMax[i] = raw[i];
  }
  r.valid = true;
  r.fieldUt = sqrtf(raw[0]*raw[0] + raw[1]*raw[1] + raw[2]*raw[2]);

  // The centre of the readings seen so far, subtracted. Before the board has
  // been turned this is close to the readings themselves and the corrected
  // values are near zero, which is why calibration is reported beside the
  // heading rather than left for the caller to infer from a wrong answer.
  float c[3];
  float span = 0.0f;
  for (int i = 0; i < 3; i++) {
    const float lo = sMin[i], hi = sMax[i];
    c[i] = raw[i] - (hi + lo) * 0.5f;
    const float s = hi - lo;
    if (s > span) span = s;
  }
  // Earth's field is 25-65 uT, so a full turn moves an axis by twice that at
  // most. Scored against 50 uT of span, which a turn on a level surface reaches
  // comfortably and a board sitting still never does.
  const float scored = span / 50.0f * 100.0f;
  r.calibration = (uint8_t)(scored > 100.0f ? 100.0f : scored);

  // Gravity says where level went. Without it the horizontal plane is assumed
  // to be the board's own, which is true only while it is held flat.
  float g[3];
  if (Imu::present() && Imu::accel(g)) {
    const float gm = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
    if (gm > 0.1f) {
      const float ax = g[0] / gm, ay = g[1] / gm, az = g[2] / gm;
      // Roll and pitch from gravity, then the field rotated back into the
      // horizontal plane by them. The standard tilt-compensated form.
      const float roll  = atan2f(ay, az);
      const float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));
      const float sr = sinf(roll),  cr = cosf(roll);
      const float sp = sinf(pitch), cp = cosf(pitch);
      const float xh = c[0]*cp + c[1]*sr*sp + c[2]*cr*sp;
      const float yh = c[1]*cr - c[2]*sr;
      r.headingDeg = atan2f(-yh, xh) * 57.29577951f;
      r.levelled = true;
      // How far from flat, for a caller that wants to say "hold it level".
      r.tiltDeg = acosf(az > 1.0f ? 1.0f : (az < -1.0f ? -1.0f : az)) * 57.29577951f;
    }
  }
  if (!r.levelled) r.headingDeg = atan2f(-c[1], c[0]) * 57.29577951f;
  if (r.headingDeg < 0.0f) r.headingDeg += 360.0f;
  return r;
}

} // namespace Compass

#endif // HAS_COMPASS
