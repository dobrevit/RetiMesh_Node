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
//  SensirionRhtMath.h — two humidity/temperature families that share a frame
//
//  The Heltec V4's third I2C part is a GXHT3V, GXCAS's clone of Sensirion's
//  SHTC3, and it answers at 0x70: the bus scan on the bench unit reads
//  `parts=3 acked=0x26 0x70 0x76` — the accelerometer, this part, and the
//  BME280. But 0x70 is the SHTC3 family's address and nothing else's, so a
//  different expansion module carrying an SHT3x would answer at 0x44 or 0x45
//  instead and speak a different command set. Both are supported here, and the
//  driver tries them in turn, so one firmware drives either module.
//
//  Why one file for two families
//  -----------------------------
//  Because they genuinely share the parts that matter: Sensirion's CRC-8, and
//  a reply shaped as a sixteen-bit word followed by its checksum, twice. What
//  differs is the command set, the identity check, and — by one — the divisor.
//  Splitting that into two files would copy the checksum into both, which is
//  the one thing this project does not do with a rule.
//
//  The divisor is not a transcription slip
//  ---------------------------------------
//  SHTC3 divides the raw count by 2^16; SHT3x divides by 2^16-1. The two
//  datasheets really do specify it differently, and the difference shows at
//  full scale: an SHT3x reads exactly 100 %, an SHTC3 a shade under. Neither
//  is rounded to meet the other, because a reading adjusted to look tidy is a
//  reading nobody can check against the part's own specification.
//
//  Fixed point, and the same units as the BME280
//  ---------------------------------------------
//  Deliberately identical to Bme280Math.h, so that two parts reporting the
//  same quantity produce the same kind of integer and can be compared without
//  a conversion standing between them:
//
//    temperature   hundredths of a degree Celsius   2314 = 23.14 C
//    humidity      1/1024 of a percent              47104 = 46.0 %
//
//  What is tested here and what is not
//  -----------------------------------
//  The checksum, the identity word and all four conversions are pure and
//  pinned by test/test_environment. Whether a number is the true temperature
//  of the room is a bench question and is on the checklist as one.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Shared: the checksum and the frame both families use
// ---------------------------------------------------------------------------
namespace SensirionRht {

// CRC-8, polynomial 0x31 (x^8 + x^5 + x^4 + 1), initialised to 0xFF, no final
// inversion, most significant bit first.
//
// This is what makes a part identifiable rather than merely present. An
// address that acknowledges says only that something is there; a pair of bytes
// whose checksum is right says the thing there speaks this protocol. The
// driver refuses any reply whose CRC does not check, which is what keeps the
// probe safe: an I2C multiplexer also lives at 0x70, and it will acknowledge
// and then fail to produce a checksummed word.
inline uint8_t crc8(const uint8_t* d, size_t n) {
  uint8_t c = 0xFF;
  for (size_t i = 0; i < n; i++) {
    c = (uint8_t)(c ^ d[i]);
    for (int bit = 0; bit < 8; bit++)
      c = (c & 0x80) ? (uint8_t)((uint8_t)(c << 1) ^ 0x31) : (uint8_t)(c << 1);
  }
  return c;
}

// A two-byte word followed by its checksum, as every reply from either family
// is shaped. False when the checksum disagrees, which is the only evidence
// available that the bytes arrived as they were sent.
inline bool wordOk(const uint8_t* p) { return crc8(p, 2) == p[2]; }

inline uint16_t word(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

// A whole six-byte measurement: temperature word, its CRC, humidity word, its
// CRC. Both halves have to check — a reply half of which survived the wire is
// not a reading, and accepting the good half would publish one real number
// beside one invented one with nothing to tell them apart.
inline bool measurementOk(const uint8_t d[6]) {
  return wordOk(d) && wordOk(d + 3);
}

} // namespace SensirionRht

// ---------------------------------------------------------------------------
// SHTC3 — and its clones, which is what the V4 carries
// ---------------------------------------------------------------------------
namespace Shtc3 {

constexpr uint8_t  kAddr     = 0x70;
constexpr uint16_t kCmdWake  = 0x3517;
constexpr uint16_t kCmdSleep = 0xB098;
constexpr uint16_t kCmdReadId = 0xEFC8;
// Temperature first, normal power, and *without* clock stretching. Without,
// because stretching holds SCL low until the conversion finishes, and this bus
// is shared — on the Heltec V4 with the DA217 accelerometer and with the
// BQ25896 charger, which another task reads. A part that can pin the bus for
// milliseconds is a part that can stall those readers. The driver waits the
// datasheet's worst case instead, which costs nothing at one reading every
// ENV_SAMPLE_MS.
constexpr uint16_t kCmdMeasure = 0x7866;
// Worst case in normal-power mode from the datasheet's timing table, rounded
// up. Waited rather than polled, because without clock stretching there is
// nothing to poll: the part simply does not acknowledge until it is ready.
//
// The driver waits the longer of this and the SHT3x's for either family, so
// the figure that is actually used is the maximum of the two — see
// Environment.cpp's kConvert2Ms, which is why this constant is defined
// alongside its sibling rather than either one being picked by hand.
constexpr uint32_t kConversionMs = 13;

// The identity word carries a product code in a scattered set of bits. The
// mask is 0x083F, which is **bit 11 and bits 5:0** — everything else, bits
// 10:6 and 15:12, is silicon revision and differs between otherwise identical
// devices. (An earlier version of this comment said "bits 11:6", which is not
// what 0x083F selects; the constants below are Sensirion's own and are right,
// only the description was wrong. Left recorded because a comment that
// misdescribes a constant is how somebody later "fixes" the constant.)
//
// Masking to the specified bits is why a clone answers this correctly —
// carrying Sensirion's product code is what being a compatible part means.
constexpr uint16_t kIdMask = 0x083F;
constexpr uint16_t kIdCode = 0x0807;

inline bool isShtc3(uint16_t id) { return (id & kIdMask) == kIdCode; }

// T [C] = -45 + 175 * raw / 2^16, in hundredths.
//
// The multiply comes before the shift and stays inside 32 bits by a
// comfortable margin: 17500 * 65535 is about 1.15e9 against a ceiling of
// 4.29e9. That order matters — shifting first throws away the fraction the
// hundredths exist to carry.
inline int32_t temperatureCentiC(uint16_t raw) {
  return -4500 + (int32_t)(((uint32_t)17500 * raw) >> 16);
}

// RH [%] = 100 * raw / 2^16, in 1/1024 of a percent. 102400 * 65535 overflows
// 32 bits, so this one widens.
inline uint32_t humidityQ1024(uint16_t raw) {
  return (uint32_t)(((uint64_t)102400 * raw) >> 16);
}

} // namespace Shtc3

// ---------------------------------------------------------------------------
// SHT3x — the other module that could be plugged into the same slot
// ---------------------------------------------------------------------------
namespace Sht3x {

// Two addresses, strapped by the ADDR pin, and both are tried for the same
// reason the BME280's pair is: the strap belongs to whichever module was
// fitted, not to the board.
constexpr uint8_t kAddrLow  = 0x44;
constexpr uint8_t kAddrHigh = 0x45;

// Single shot, high repeatability, clock stretching disabled — the same
// argument as the SHTC3's, and on the same shared bus.
constexpr uint16_t kCmdMeasure = 0x2400;
// This family has no product-id command that every variant implements, so the
// status register stands in as the identity check: it is universally
// supported, and its reply is checksummed, which is the proof that matters.
constexpr uint16_t kCmdStatus  = 0xF32D;
// High-repeatability worst case from the datasheet, rounded up.
constexpr uint32_t kConversionMs = 16;

// T [C] = -45 + 175 * raw / (2^16 - 1), in hundredths. Not a shift, because
// the divisor is not a power of two — see the header's note on why this
// family differs from the SHTC3 by exactly one.
inline int32_t temperatureCentiC(uint16_t raw) {
  return -4500 + (int32_t)(((uint32_t)17500 * raw) / 65535u);
}

// RH [%] = 100 * raw / (2^16 - 1), in 1/1024 of a percent. Widened for the
// same overflow reason as the SHTC3's.
inline uint32_t humidityQ1024(uint16_t raw) {
  return (uint32_t)(((uint64_t)102400 * raw) / 65535u);
}

} // namespace Sht3x
