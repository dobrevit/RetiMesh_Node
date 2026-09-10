// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.


// ============================================================================
//  Bme280Math.h — a BME280's readings are not readings until they are computed
//
//  The part hands over three raw converter counts and eleven calibration
//  coefficients burned into it at the factory, and the numbers a person wants
//  are a polynomial away. That polynomial is the part's specification — Bosch
//  publish it in the BME280 datasheet (BST-BME280-DS002, the compensation
//  formulae section) as the way the device is to be read, in the same sense
//  that a register map is — so the arithmetic below is theirs and the
//  expression of it is this file's. Nothing here is tidied: every divisor is a
//  power of two the part chose, and an expression rearranged to look neater is
//  a different function that happens to resemble this one.
//
//  Fixed point, not floating
//  -------------------------
//  The datasheet gives both, and this is the integer path. Two reasons, and
//  the second is the one that matters. The ESP32's FPU is single precision, so
//  the double formulae are software emulation on this target — cheap at one
//  reading every half minute, but paid for nothing. And integer arithmetic has
//  no tolerance: a test can assert the answer rather than assert it is close,
//  which is what makes the decode rules below testable at all.
//
//  So the units here are the datasheet's own, and they are not SI:
//
//    temperature   hundredths of a degree Celsius   2314 = 23.14 C
//    pressure      hundredths of a pascal           10132500 = 1013.25 hPa
//    humidity      1/1024 of a percent              47104 = 46.0 %
//
//  Callers convert at the surface they print on, once, rather than this file
//  handing out floats that every caller then has to guess the scale of.
//
//  What is tested and what is not
//  ------------------------------
//  test/test_environment pins the parts that can be got wrong silently: the
//  calibration decode, where h4 and h5 are packed across a shared byte's two
//  nibbles and where six of the eleven coefficients are signed; the assembly
//  of a 20-bit converter count out of three registers; the clamps; and the
//  divide-by-zero the pressure polynomial guards against. Those are rules with
//  an authority outside this file.
//
//  What is *not* tested here is whether the compensated number is the true
//  temperature of the room, because nothing on a host can know that. The
//  polynomial is transcribed and reviewed by reading; that it produces a
//  plausible reading, and one that agrees with another instrument, is a bench
//  question and is on the checklist as one.
// ============================================================================
#pragma once

#include <stdint.h>

namespace Bme280 {

// What answered at an address, decided from the chip id register alone.
//
// Here rather than in the driver because it is a rule with three outcomes and
// no hardware in it, and because the probe below it now asks the question at
// more than one address: a board whose part arrives on a plug-in module does
// not know which of the two straps the module used, so something has to say
// what counts as "that is the part" — and saying it once, testably, is what
// keeps a probe from accepting a neighbour that happens to answer.
enum class Part : uint8_t {
  None   = 0,   // nothing, or something that is not a Bosch pressure part
  Bme280 = 1,   // temperature, pressure and humidity
  Bmp280 = 2,   // the same part without the humidity half
};

// `id` is the byte read from register 0xD0, or a negative value when the read
// itself failed — which is the ordinary answer at an address with nothing on
// it, and must not be confused with a part that answered something unexpected.
inline Part identify(int id) {
  if (id < 0) return Part::None;
  if (id == 0x60) return Part::Bme280;
  if (id == 0x58) return Part::Bmp280;
  return Part::None;
}


// The eleven coefficients, as the part reports them. Named for the datasheet's
// own names rather than anything more descriptive, because the polynomial is
// written in those names and a reader checking one against the other should
// not have to translate.
struct Calibration {
  uint16_t t1 = 0;
  int16_t  t2 = 0, t3 = 0;
  uint16_t p1 = 0;
  int16_t  p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0, p7 = 0, p8 = 0, p9 = 0;
  uint8_t  h1 = 0;
  int16_t  h2 = 0;
  uint8_t  h3 = 0;
  int16_t  h4 = 0, h5 = 0;
  int8_t   h6 = 0;
};

// Three converter counts: temperature and pressure are 20-bit, humidity 16.
struct Raw {
  uint32_t temperature = 0;
  uint32_t pressure    = 0;
  uint32_t humidity    = 0;
};

// The calibration arrives as two blocks, because the part's register map puts
// them in two places: twenty-six bytes from 0x88 and seven from 0xe1.
//
// Two things in here are worth more attention than the rest of this file. Six
// of these coefficients are *signed* and the polynomial is wrong in a way that
// looks plausible if any of them is read unsigned — a temperature that drifts
// the wrong way at one end of the range, not a number that is obviously
// broken. And h4 and h5 share a byte: h4 takes the high eight bits from 0xe4
// and its low four from the low nibble of 0xe5, h5 takes the high eight from
// 0xe6 and its low four from the *high* nibble of that same 0xe5. Getting the
// two nibbles the wrong way round is the classic error with this part, and it
// shows up as a humidity that is merely wrong rather than absent.
inline Calibration decodeCalibration(const uint8_t first[26], const uint8_t second[7]) {
  Calibration c;
  auto u16 = [](const uint8_t* p) -> uint16_t {
    return (uint16_t)((uint16_t)p[1] << 8 | p[0]);            // little endian
  };
  auto s16 = [&](const uint8_t* p) -> int16_t { return (int16_t)u16(p); };

  c.t1 = u16(first + 0);
  c.t2 = s16(first + 2);
  c.t3 = s16(first + 4);
  c.p1 = u16(first + 6);
  c.p2 = s16(first + 8);
  c.p3 = s16(first + 10);
  c.p4 = s16(first + 12);
  c.p5 = s16(first + 14);
  c.p6 = s16(first + 16);
  c.p7 = s16(first + 18);
  c.p8 = s16(first + 20);
  c.p9 = s16(first + 22);
  // first[24] is unused padding in the part's map; h1 is the last byte.
  c.h1 = first[25];

  c.h2 = s16(second + 0);
  c.h3 = second[2];
  // The shared byte is second[4] (register 0xe5). The high bytes are sign
  // extended through int8_t before being shifted, which is what makes a
  // negative coefficient come out negative.
  c.h4 = (int16_t)(((int16_t)(int8_t)second[3] * 16) | (int16_t)(second[4] & 0x0F));
  c.h5 = (int16_t)(((int16_t)(int8_t)second[5] * 16) | (int16_t)(second[4] >> 4));
  c.h6 = (int8_t)second[6];
  return c;
}

// The eight bytes of one burst read from 0xf7: pressure, temperature and
// humidity in that order, the first two as msb/lsb/xlsb where only the top
// four bits of the xlsb carry data.
// Whether a calibration block is a real one the part gave up, or a transfer
// that half worked.
//
// Here rather than in the driver for the same reason identify() is: it is a
// decision over data the caller already holds, with no bus in it, and it is
// the second half of what makes a two-address probe safe — a part is accepted
// only when it answers the right chip id *and* hands over coefficients that
// could have come from silicon.
//
// All three are unsigned in the datasheet's table and none is zero on any real
// device. t1 alone would not be enough: a zero p1 is the *divisor* in the
// pressure polynomial below, whose guard returns the 300 hPa clamp, so a part
// accepted with one would publish the lowest pressure ever recorded, steadily,
// for as long as the node ran. A sensor reporting nothing is better than that,
// because only one of the two makes anybody look at it.
inline bool calibrationLooksReal(const Calibration& c) {
  return c.t1 != 0 && c.p1 != 0 && c.h1 != 0;
}

inline Raw decodeRaw(const uint8_t d[8]) {
  Raw r;
  r.pressure    = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4);
  r.temperature = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4);
  r.humidity    = ((uint32_t)d[6] << 8)  |  (uint32_t)d[7];
  return r;
}

// The intermediate the other two readings are computed from, and the reason
// this is a separate call: pressure and humidity are both functions of the
// temperature at the moment of the conversion, so all three come from one
// burst read or they describe three different moments.
inline int32_t tFine(const Calibration& c, uint32_t adcT) {
  const int32_t t = (int32_t)adcT;
  int32_t a = (t / 8) - ((int32_t)c.t1 * 2);
  a = (a * (int32_t)c.t2) / 2048;
  int32_t b = (t / 16) - (int32_t)c.t1;
  b = (((b * b) / 4096) * (int32_t)c.t3) / 16384;
  return a + b;
}

// Hundredths of a degree, clamped to the range the part is specified over. The
// clamp is the datasheet's and it is not cosmetic: outside it the polynomial
// is not calibrated, so a number from beyond the edge is not a colder reading,
// it is arithmetic about nothing.
inline int32_t temperatureCentiC(int32_t tFineValue) {
  int32_t t = (tFineValue * 5 + 128) / 256;
  if (t < -4000) t = -4000;
  if (t > 8500)  t = 8500;
  return t;
}

// Hundredths of a pascal. 64-bit throughout, because the polynomial's
// intermediates leave 32 bits long before the answer does.
inline uint32_t pressureCentiPa(const Calibration& c, int32_t tFineValue, uint32_t adcP) {
  const uint32_t kMin = 3000000, kMax = 11000000;
  int64_t v1 = (int64_t)tFineValue - 128000;
  int64_t v2 = v1 * v1 * (int64_t)c.p6;
  v2 += (v1 * (int64_t)c.p5) * 131072;
  v2 += (int64_t)c.p4 * 34359738368LL;
  v1 = ((v1 * v1 * (int64_t)c.p3) / 256) + (v1 * (int64_t)c.p2 * 4096);
  v1 = (140737488355328LL + v1) * (int64_t)c.p1 / 8589934592LL;
  // A part whose p1 read back as zero, or a calibration block that never
  // arrived, divides by nothing here. The datasheet's own code guards it and
  // so does this: the floor is the honest answer, because there is no reading.
  if (v1 == 0) return kMin;
  // The cast is explicit where the datasheet's C leaves it implicit. There the
  // subtraction is int minus unsigned and would wrap for a count above
  // 1048576; the count is twenty bits so it never does, and saying so in the
  // type is cheaper than relying on a domain to hide a promotion.
  int64_t v4 = 1048576LL - (int64_t)adcP;
  v4 = ((v4 * 2147483648LL) - v2) * 3125 / v1;
  v1 = ((int64_t)c.p9 * (v4 / 8192) * (v4 / 8192)) / 33554432;
  v2 = ((int64_t)c.p8 * v4) / 524288;
  v4 = ((v4 + v1 + v2) / 256) + ((int64_t)c.p7 * 16);
  int64_t p = ((v4 / 2) * 100) / 128;
  if (p < (int64_t)kMin) return kMin;
  if (p > (int64_t)kMax) return kMax;
  return (uint32_t)p;
}

// Thousandths — 1/1024ths, rather — of a percent. The intermediate clamp at
// 419430400 is the datasheet's and comes before the shift, so a saturated
// reading lands exactly on 100 % instead of overflowing past it.
inline uint32_t humidityQ1024(const Calibration& c, int32_t tFineValue, uint32_t adcH) {
  const uint32_t kMax = 102400;                 // 100 % in these units
  const int32_t v1 = tFineValue - 76800;
  int32_t a = (int32_t)(adcH * 16384);
  int32_t b = (int32_t)c.h4 * 1048576;
  int32_t d = (int32_t)c.h5 * v1;
  const int32_t e = ((a - b) - d + 16384) / 32768;
  a = (v1 * (int32_t)c.h6) / 1024;
  b = (v1 * (int32_t)c.h3) / 2048;
  d = ((a * (b + 32768)) / 1024) + 2097152;
  a = ((d * (int32_t)c.h2) + 8192) / 16384;
  b = e * a;
  d = ((b / 32768) * (b / 32768)) / 128;
  int32_t h = b - ((d * (int32_t)c.h1) / 16);
  if (h < 0) h = 0;
  if (h > 419430400) h = 419430400;
  const uint32_t out = (uint32_t)h / 4096;
  return out > kMax ? kMax : out;
}

} // namespace Bme280
