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
//  MagHeading.h — a field vector into a bearing, and how much to believe it
//
//  Three pieces of arithmetic that used to sit inside Compass.cpp's sample(),
//  lifted out for the reason Bme280Math.h and GeoMath.h were: they are floats
//  in and floats out, every way of getting them wrong is silent and plausible,
//  and a bench is the worst place to find any of it.
//
//  Lifted out and, in one place, corrected — which is what came of being able
//  to test it. The tilt compensation had gravity's sign convention wrong and
//  returned a bearing reflected about north for a board lying flat; the first
//  test written against the stated convention failed, and tilted() below says
//  what the fix is. Everything else is the same arithmetic to the digit.
//
//  What "silent and plausible" means here, concretely. Scoring the calibration
//  on the *best* of the three axes instead of the worst of two reports a
//  trustworthy compass after the board has been tipped end over end, which
//  swings Z through the whole field while X and Y — the pair the bearing is
//  computed from — have never moved. A sign slip in the tilt rotation gives a
//  heading that is steady, turns with the board, and is wrong by tens of
//  degrees at any angle off flat. A divisor of 25 instead of 50 reports full
//  calibration at half a turn. None of those looks like a fault; all of them
//  are decidable on a host against a vector worked out by hand.
//
//  The convention is the one Compass.cpp has always used and is preserved to
//  the digit: x forward, y right, z down in the part's own frame, and a bearing
//  of atan2(-y, x) measured clockwise from the x axis, wrapped into 0-360. It
//  is a *magnetic* bearing — declination is the caller's business (Compass.h).
// ============================================================================
#pragma once

#include <math.h>
#include <stdint.h>

namespace MagHeading {

// Degrees per radian, spelled the way Compass.cpp spelled it so that lifting
// this arithmetic out changed no digit of it.
constexpr float kDegPerRad = 57.29577951f;

// How much of a turn the hard-iron extremes have seen, 0-100.
//
// The offsets are the middle of the extremes each axis has reached, so the
// spread of an axis is what says whether that axis has been anywhere. Scored on
// the worse of x and y — the two the bearing turns on — and deliberately not on
// the best of three: Earth's field is 25-65 uT, so turning an axis right around
// swings it by twice the local horizontal component, and fifty microtesla of
// spread is what a full turn on a level surface reaches while a board sitting
// still never approaches it.
inline uint8_t calibrationScore(const float min[3], const float max[3]) {
  const float sx = max[0] - min[0];
  const float sy = max[1] - min[1];
  const float weakest = sx < sy ? sx : sy;
  const float scored = weakest / 50.0f * 100.0f;
  return (uint8_t)(scored > 100.0f ? 100.0f : (scored < 0.0f ? 0.0f : scored));
}

// The reading with the board's own field removed: the centre of the extremes
// seen so far, subtracted. Before the board has been turned this is close to
// the readings themselves and the corrected values are near zero, which is why
// the score above is reported beside the heading rather than left to be
// inferred from a wrong answer.
inline void removeHardIron(const float raw[3], const float min[3],
                           const float max[3], float out[3]) {
  for (int i = 0; i < 3; i++) out[i] = raw[i] - (max[i] + min[i]) * 0.5f;
}

// Everything a bearing needs to be judged. `levelled` false means gravity was
// not available and the horizontal plane was assumed to be the board's own,
// which is true only while it is held flat; `tiltDeg` is then 0 because it is
// not a measurement of anything rather than a measurement of nothing.
struct Bearing {
  float headingDeg = 0.0f;
  float tiltDeg    = 0.0f;
  bool  levelled   = false;
};

// A bearing from the corrected field alone, for a board held flat.
inline Bearing flat(const float c[3]) {
  Bearing b;
  b.headingDeg = atan2f(-c[1], c[0]) * kDegPerRad;
  if (b.headingDeg < 0.0f) b.headingDeg += 360.0f;
  return b;
}

// A bearing with the field rotated back into the horizontal plane by the roll
// and pitch that gravity implies — the standard tilt-compensated form. Falls
// back to flat() when the gravity vector is too short to say which way is
// down, because a normalised vector out of nothing is a heading out of
// nothing.
inline Bearing tilted(const float c[3], const float g[3]) {
  const float gm = sqrtf(g[0]*g[0] + g[1]*g[1] + g[2]*g[2]);
  if (!(gm > 0.1f)) return flat(c);

  // Gravity, normalised, and then *pointed the same way whichever sign the
  // part reports it with*. This is the correction's one convention and it was
  // missing: the standard form is derived for an accelerometer that reads
  // +1 g on the axis pointing down, and the parts here read −1 g on z lying
  // face up (Imu.cpp). Fed −1, atan2f(ay, az) returns 180 degrees rather than
  // 0, the rotation below mirrors y, and a board lying flat gets a bearing
  // reflected about north — 315 where the same field read flat gives 45.
  //
  // That is not a small error and it is not visible on a bench: the heading is
  // steady, it turns as the board turns, and it is wrong. It shipped that way
  // on the board that had a compass first, because nothing compared the two
  // paths in the one case where they must agree. Taking the sign out here
  // makes them agree by construction, and test_mag_heading pins it.
  //
  // Past ninety degrees of tilt a bearing from gravity alone is ambiguous
  // whatever the sign convention — two angles cannot describe three — and this
  // flip resolves the ambiguity towards "the board is roughly the way up it
  // was built to be", which is the case anybody reads a heading in.
  const float flip = g[2] < 0.0f ? -1.0f : 1.0f;
  const float ax = flip * g[0] / gm, ay = flip * g[1] / gm, az = flip * g[2] / gm;
  const float roll  = atan2f(ay, az);
  const float pitch = atan2f(-ax, sqrtf(ay*ay + az*az));
  const float sr = sinf(roll),  cr = cosf(roll);
  const float sp = sinf(pitch), cp = cosf(pitch);
  const float xh = c[0]*cp + c[1]*sr*sp + c[2]*cr*sp;
  const float yh = c[1]*cr - c[2]*sr;

  Bearing b;
  b.headingDeg = atan2f(-yh, xh) * kDegPerRad;
  if (b.headingDeg < 0.0f) b.headingDeg += 360.0f;
  b.levelled = true;
  // How far from flat, from the *magnitude* of the vertical component and not
  // its signed value: which way up the board is does not change how level it
  // is. The flip above already makes az positive, and the absolute value is
  // kept anyway — it is what makes this line true on its own terms rather than
  // true because of something eight lines earlier.
  const float vertical = az < 0.0f ? -az : az;
  b.tiltDeg = acosf(vertical > 1.0f ? 1.0f : vertical) * kDegPerRad;
  return b;
}

// The magnitude of the field as measured, hard iron included — the number that
// says something magnetic is close, since Earth's own is 25-65 uT.
inline float magnitude(const float v[3]) {
  return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

} // namespace MagHeading
