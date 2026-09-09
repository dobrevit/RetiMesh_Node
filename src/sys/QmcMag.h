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
//  QmcMag.h — what to write to a QMC6310, and what its counts are worth
//
//  QST's magnetometers share a register map and disagree about the bits inside
//  it. The QMC6309 the ThinkNode M9 carries and the QMC6310N on the T-Beam
//  Supreme both answer a chip id at 0x00, both put six little-endian output
//  bytes at 0x01, both take a mode in CTRL1 (0x0A) and a range in CTRL2
//  (0x0B) — and the fields inside those two registers are laid out
//  differently, as are the counts per microtesla. So the parts are one driver
//  (Compass.cpp) and two sets of numbers, and the numbers live here.
//
//  Here rather than in the driver because every way of getting them wrong is
//  silent and plausible. A range field one value off scales every reading by
//  four and still reports a steady heading that turns with the board; the
//  wrong oversampling bits cost noise nobody can see in a number; a mode
//  written into the range field suspends a part that goes on answering its
//  address. None of that is a crash, none of it shows up on a bench without a
//  reference field, and all of it is arithmetic a host can check.
//
//  The 6310's field definitions are QST's, taken from the driver LilyGO ship
//  for this board (SensorLib's SensorQSTMagnetic.hpp) — which is also the code
//  that was run against this unit to prove the part works, so the
//  configuration this header calls "verified" is verified in the strict sense:
//  those exact register values produced the readings in docs/hardware.md.
//
//  The 6309's two run bytes are *not* derived here. They are the bench-proven
//  constants the M9 has shipped with, and this header states them without
//  taking them apart: no datasheet for that part's field layout has been read,
//  and inventing a derivation that happens to produce the right byte would be
//  a comment that looks like knowledge. Compass.cpp uses them the same way it
//  always has.
// ============================================================================
#pragma once

#include <stdint.h>

namespace QmcMag {

// ---------------------------------------------------------------------------
// The register map both parts share
// ---------------------------------------------------------------------------
constexpr uint8_t kRegChipId = 0x00;
constexpr uint8_t kRegDataX  = 0x01;   // six bytes: x, y, z, each low byte first
constexpr uint8_t kRegStatus = 0x09;   // bit 0 data ready, bit 1 overflow
constexpr uint8_t kRegCtrl1  = 0x0A;
constexpr uint8_t kRegCtrl2  = 0x0B;

constexpr uint8_t kStatusDataReady = 0x01;
constexpr uint8_t kStatusOverflow  = 0x02;

// A soft reset, and the byte that stops a part measuring. Both are the same on
// the two parts: CTRL2's top bit resets, and CTRL1 with every bit clear is the
// state a part powers up in — which on the 6310 is mode "suspend" and on the
// 6309 was read off a bench before anything had configured it.
constexpr uint8_t kSoftReset    = 0x80;
constexpr uint8_t kCtrl1Suspend = 0x00;

// ---------------------------------------------------------------------------
// The chip ids, which are how the driver tells the parts apart
// ---------------------------------------------------------------------------
constexpr uint8_t kChipIdQmc6309 = 0x90;
constexpr uint8_t kChipIdQmc6310 = 0x80;

// ---------------------------------------------------------------------------
// The QMC6309's run bytes: bench-proven, not derived. See the file comment.
// ---------------------------------------------------------------------------
constexpr uint8_t kQmc6309Ctrl1Run = 0xD3;
constexpr uint8_t kQmc6309Ctrl2Run = 0x03;
constexpr float   kQmc6309UtPerCount = 0.0488f;

// ---------------------------------------------------------------------------
// The QMC6310's fields
// ---------------------------------------------------------------------------
// CTRL1 packs four of them and CTRL2 one. Each is written as an enum rather
// than a shifted literal so that a call site says which knob it is turning,
// and so that a test can prove the fields do not overlap — the failure that
// matters here is one field's value landing in another's bits, which produces
// a part that runs in a mode nobody chose.
enum class Mode : uint8_t {
  Suspend    = 0,      // powers up here; stops converting and keeps answering
  Normal     = 1,
  Single     = 2,
  Continuous = 3,
};

enum class Odr : uint8_t { Hz10 = 0, Hz50 = 1, Hz100 = 2, Hz200 = 3 };

// Oversampling: the part averages this many conversions internally before it
// presents one. X8 is the quietest and X1 the fastest — and X1 is what the
// verified configuration below uses, because it is what LilyGO's example set
// when this part was proven on this board. The knob is here so a later change
// is one argument and one test, rather than a new magic byte.
enum class Osr : uint8_t { X8 = 0, X4 = 1, X2 = 2, X1 = 3 };

// Downsampling, applied after oversampling. X1 is none.
enum class Dsr : uint8_t { X1 = 0, X2 = 1, X4 = 2, X8 = 3 };

// Full scale. 8 G is 800 uT against an Earth field of 25-65 uT, which sounds
// like waste until the board's own hard iron is counted: the field this part
// sits in on the Supreme reads about 120 uT with a large offset on Z, and a
// range that clips is a heading that locks rather than a reading that saturates
// visibly.
enum class Range : uint8_t { G30 = 0, G12 = 1, G8 = 2, G2 = 3 };

// CTRL1: mode in bits 1:0, output rate in 3:2, oversampling in 5:4,
// downsampling in 7:6.
constexpr uint8_t ctrl1(Mode m, Odr odr, Osr osr, Dsr dsr) {
  return (uint8_t)(((uint8_t)m   & 0x03)       |
                   (((uint8_t)odr & 0x03) << 2) |
                   (((uint8_t)osr & 0x03) << 4) |
                   (((uint8_t)dsr & 0x03) << 6));
}

// CTRL2: range in bits 3:2. Bit 6 is the part's self-test and bit 7 its soft
// reset; neither belongs in a run value, and both are left clear here.
constexpr uint8_t ctrl2(Range r) {
  return (uint8_t)(((uint8_t)r & 0x03) << 2);
}

// Microtesla per count, from QST's sensitivity table in LSB per gauss — 1000
// at 30 G, 2500 at 12 G, 3750 at 8 G, 15000 at 2 G — and one gauss is a
// hundred microtesla.
constexpr float utPerCount(Range r) {
  return r == Range::G30 ? 100.0f / 1000.0f
       : r == Range::G12 ? 100.0f / 2500.0f
       : r == Range::G8  ? 100.0f / 3750.0f
       :                   100.0f / 15000.0f;
}

// One axis, from its two output bytes. Little-endian, signed, and the sign
// matters: a magnetometer reads negative on any axis pointing away from the
// field, which is most of them most of the time.
constexpr int16_t axisCounts(uint8_t low, uint8_t high) {
  return (int16_t)((uint16_t)low | ((uint16_t)high << 8));
}

constexpr float axisUt(int16_t counts, Range r) {
  return (float)counts * utPerCount(r);
}

// The signedness above is the whole point of that function and is asserted
// rather than left to the tests, because every call site narrows the result
// into an int16_t of its own and would reproduce the right value even if this
// returned unsigned — which is to say the mistake is invisible at the call
// site and has to be caught here.
static_assert(sizeof(axisCounts(0, 0)) == 2, "axisCounts must return 16 bits");
static_assert((int)axisCounts(0x00, 0xFB) == -1280,
              "axisCounts must be signed: read unsigned, -1280 becomes 64256 "
              "and the field reads 1713 uT, which looks like a broken part");

// ---------------------------------------------------------------------------
// The configuration the Supreme's part was proven in
// ---------------------------------------------------------------------------
// Continuous at 200 Hz, no oversampling, no downsampling, 8 G. LilyGO's
// QMC63xx_GetDataExample set exactly this on the unit here and its output is
// quoted in docs/hardware.md — 96 counts reported as 2.56 uT, which is this
// range's scale to three figures and is pinned as a test vector.
//
// 200 Hz against a driver that samples at 10 Hz is deliberate, and the same
// choice the 6309 path makes: the part is always holding a conversion that
// finished a few milliseconds ago, so a sample never waits and never catches
// one half-made.
constexpr Range kRange     = Range::G8;
constexpr uint8_t kQmc6310Ctrl1Run = ctrl1(Mode::Continuous, Odr::Hz200,
                                           Osr::X1, Dsr::X1);
constexpr uint8_t kQmc6310Ctrl2Run = ctrl2(kRange);
constexpr float   kQmc6310UtPerCount = utPerCount(kRange);

} // namespace QmcMag
