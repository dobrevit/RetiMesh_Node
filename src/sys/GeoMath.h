// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  GeoMath.h — distance and bearing between two fixes
//
//  Haversine great-circle distance and initial bearing on the WGS84 mean
//  sphere. Header-only and Arduino-free: the host suite pins it to vectors
//  a reader can verify by hand (a degree of latitude, the equator's degree
//  of longitude), which is the only way to trust the math a search party
//  might follow.
// ============================================================================
#pragma once

#include <math.h>

namespace GeoMath {

constexpr double kEarthRadiusKm = 6371.0088;   // IUGG mean radius

inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
  const double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
  const double dp = (lat2 - lat1) * M_PI / 180.0;
  const double dl = (lon2 - lon1) * M_PI / 180.0;
  const double a = sin(dp / 2) * sin(dp / 2) +
                   cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2.0 * kEarthRadiusKm * atan2(sqrt(a), sqrt(1.0 - a));
}

// Initial bearing from point 1 toward point 2, degrees clockwise from true
// north, 0..360.
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
  const double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
  const double dl = (lon2 - lon1) * M_PI / 180.0;
  const double y = sin(dl) * cos(p2);
  const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
  double b = atan2(y, x) * 180.0 / M_PI;
  if (b < 0) b += 360.0;
  return b;
}

// The pair as every screen wants it: shared conversions computed once —
// the S3 does doubles in software, and the two were only ever called
// together.
inline void distanceAndBearing(double lat1, double lon1, double lat2, double lon2,
                               double& km, double& deg) {
  const double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
  const double dl = (lon2 - lon1) * M_PI / 180.0;
  const double sindl = sin(dl), cosdl = cos(dl);
  const double cosp1 = cos(p1), cosp2 = cos(p2);
  const double dp = p2 - p1;
  const double a = sin(dp / 2) * sin(dp / 2) + cosp1 * cosp2 * sin(dl / 2) * sin(dl / 2);
  km = 2.0 * kEarthRadiusKm * atan2(sqrt(a), sqrt(1.0 - a));
  const double y = sindl * cosp2;
  const double x = cosp1 * sin(p2) - sin(p1) * cosp2 * cosdl;
  deg = atan2(y, x) * 180.0 / M_PI;
  if (deg < 0) deg += 360.0;
}

} // namespace GeoMath
