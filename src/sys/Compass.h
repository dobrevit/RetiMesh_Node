// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Compass.h — which way the node is pointing
//
//  A magnetometer measures the field it is sitting in, and the field it is
//  sitting in is the Earth's plus the board's own. Turning that into a heading
//  needs three things, and leaving any of them out gives a number that looks
//  like a heading and is not one:
//
//  Tilt. Field is a vector in three dimensions and a heading is an angle in
//  two, so the reading has to be projected onto the horizontal plane. Held
//  level, x and y are already that plane and the arctangent of them is the
//  answer. Held at any other angle it is not, and the error is large — tens of
//  degrees for a tilt you would not notice holding the board. Gravity, from the
//  accelerometer, is what says where the horizontal plane went.
//
//  Hard iron. Anything magnetised on the board — the speaker, the cell, the
//  antenna's own currents — adds a constant offset to every reading, which
//  moves the centre of the sphere the readings lie on away from the origin. The
//  arctangent is taken about the origin, so an offset centre turns into a
//  heading error that varies with direction: right in two places, worst at
//  ninety degrees from them. It is removed by watching the extremes each axis
//  reaches and taking the middle, which costs six numbers and needs the board
//  to have been turned around at least once. Until it has, `calibration` says
//  so rather than the heading pretending.
//
//  Declination. This reports magnetic north. True north differs by up to tens
//  of degrees depending where on Earth the node is, and the node knows where it
//  is from GNSS — but the correction needs a world model this firmware does not
//  carry, so it is left to whatever reads this, said plainly rather than
//  quietly folded in wrong.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_COMPASS

#include <stdint.h>

namespace Compass {

struct Reading {
  bool    valid       = false;   // the magnetometer answered
  bool    levelled    = false;   // ...and the accelerometer did, so tilt was removed
  float   headingDeg  = 0.0f;    // 0-360 from magnetic north, clockwise
  float   tiltDeg     = 0.0f;    // how far off level the board is held
  float   magUt[3]    = {0,0,0}; // the field as measured, hard iron included
  float   fieldUt     = 0.0f;    // its magnitude; wildly off means something magnetic is near
  uint8_t calibration = 0;       // 0-100: how much of a turn the offsets have seen
};

void begin();
bool present();

// Take a sample, whether or not anybody wants the answer. This is what makes
// the hard-iron offsets findable: they are the middle of the extremes each axis
// reaches, and the extremes are only reached while the board is being turned —
// which is exactly when nobody is looking at a console. Sampling only on demand
// meant a full turn contributed the two readings either side of it and nothing
// in between, so the calibration could never fill in however much the board was
// moved. Call it from the loop; it is one short transaction.
void poll();

// The most recent sample, taken now if the last one is stale.
Reading read();

} // namespace Compass

#else
namespace Compass {
struct Reading {
  bool    valid = false, levelled = false;
  float   headingDeg = 0.0f, tiltDeg = 0.0f, magUt[3] = {0,0,0}, fieldUt = 0.0f;
  uint8_t calibration = 0;
};
inline void begin() {}
inline bool present() { return false; }
inline void poll() {}
inline Reading read() { return Reading{}; }
} // namespace Compass
#endif // HAS_COMPASS
