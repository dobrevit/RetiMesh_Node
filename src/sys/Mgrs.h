// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Mgrs.h — a position in the words a grid user works in
//
//  WGS84 lat/lon to MGRS at one-metre precision: UTM zone (with the Norway
//  and Svalbard exceptions), transverse Mercator easting/northing, the
//  latitude band, and the AA-scheme 100 km square letters. Header-only and
//  Arduino-free on purpose: the host suite proves it against a published
//  reference point, which is the only way to trust grid math.
// ============================================================================
#pragma once

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

namespace Mgrs {

inline bool fromLatLon(double lat, double lon, char* out, size_t n) {
  if (lat < -80.0 || lat > 84.0 || lon < -180.0 || lon > 180.0) return false;
  int zone = (int)floor((lon + 180.0) / 6.0) + 1;
  if (zone > 60) zone = 60;
  if (lat >= 56.0 && lat < 64.0 && lon >= 3.0 && lon < 12.0) zone = 32;   // Norway
  if (lat >= 72.0 && lat < 84.0) {                                        // Svalbard
    if      (lon >= 0.0  && lon < 9.0 ) zone = 31;
    else if (lon >= 9.0  && lon < 21.0) zone = 33;
    else if (lon >= 21.0 && lon < 33.0) zone = 35;
    else if (lon >= 33.0 && lon < 42.0) zone = 37;
  }

  const double a = 6378137.0, f = 1.0 / 298.257223563, k0 = 0.9996;
  const double e2 = f * (2.0 - f), ep2 = e2 / (1.0 - e2);
  const double latR = lat * M_PI / 180.0;
  const double lonR = lon * M_PI / 180.0;
  const double lon0 = (((double)zone - 1.0) * 6.0 - 180.0 + 3.0) * M_PI / 180.0;
  const double sinL = sin(latR), cosL = cos(latR), tanL = tan(latR);
  const double N = a / sqrt(1.0 - e2 * sinL * sinL);
  const double T = tanL * tanL;
  const double C = ep2 * cosL * cosL;
  const double A = cosL * (lonR - lon0);
  const double M = a * ((1 - e2/4 - 3*e2*e2/64 - 5*e2*e2*e2/256) * latR
                 - (3*e2/8 + 3*e2*e2/32 + 45*e2*e2*e2/1024) * sin(2*latR)
                 + (15*e2*e2/256 + 45*e2*e2*e2/1024) * sin(4*latR)
                 - (35*e2*e2*e2/3072) * sin(6*latR));
  const double easting = k0 * N * (A + (1-T+C)*A*A*A/6
                       + (5 - 18*T + T*T + 72*C - 58*ep2)*A*A*A*A*A/120) + 500000.0;
  double northing = k0 * (M + N * tanL * (A*A/2 + (5 - T + 9*C + 4*C*C)*A*A*A*A/24
                  + (61 - 58*T + T*T + 600*C - 330*ep2)*A*A*A*A*A*A/720));
  if (lat < 0) northing += 10000000.0;

  static const char kBands[] = "CDEFGHJKLMNPQRSTUVWX";
  int band = (int)floor((lat + 80.0) / 8.0);
  if (band > 19) band = 19;

  static const char kColSets[3][9] = { "ABCDEFGH", "JKLMNPQR", "STUVWXYZ" };
  const int colIdx = (int)(easting / 100000.0) - 1;
  if (colIdx < 0 || colIdx > 7) return false;
  static const char kRows[] = "ABCDEFGHJKLMNPQRSTUV";   // 20, no I or O
  int rowIdx = (int)fmod(floor(northing / 100000.0), 20.0);
  if ((zone % 2) == 0) rowIdx = (rowIdx + 5) % 20;      // even zones start at F

  const unsigned e5 = (unsigned)((uint64_t)easting  % 100000u);
  const unsigned n5 = (unsigned)((uint64_t)northing % 100000u);
  snprintf(out, n, "%d%c %c%c %05u %05u", zone, kBands[band],
           kColSets[(zone - 1) % 3][colIdx], kRows[rowIdx], e5, n5);
  return true;
}

} // namespace Mgrs
