// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  PeerPositions.h — where the mesh last said its peers were
//
//  Positions arrive inside message telemetry (Sideband's convention, and our
//  own Telemetry module's); this keeps the last one per peer with the local
//  time it arrived, so the bearing dial and the plot can be honest about
//  staleness. Glass-only, like every store that exists for a screen.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "Config.h"

#if HAS_LVGL_UI
namespace PeerPositions {

struct Position {
  double   latitude = 0.0, longitude = 0.0;
  float    altitudeM = 0.0f;
  float    accuracyM = 0.0f;
  uint32_t heardMs = 0;                  // our clock, when it arrived
};

void seen(const uint8_t hash[16], const Position& p);
bool get(const uint8_t hash[16], Position& out);
bool getByHex(const char* hashHex, Position& out);
size_t count();

} // namespace PeerPositions
#else
namespace PeerPositions {
struct Position {
  double latitude = 0.0, longitude = 0.0;
  float altitudeM = 0.0f, accuracyM = 0.0f;
  uint32_t heardMs = 0;
};
inline void seen(const uint8_t*, const Position&) {}
inline bool get(const uint8_t*, Position&) { return false; }
inline bool getByHex(const char*, Position&) { return false; }
inline size_t count() { return 0; }
} // namespace PeerPositions
#endif
