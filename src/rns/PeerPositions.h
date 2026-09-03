// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  PeerPositions.h — where the mesh last said its peers were
//
//  Positions arrive inside verified messages' telemetry; this keeps the last
//  one per peer with the local time it arrived, so the instruments can be
//  honest about staleness. Compiled only where both a glass and a receiver
//  exist — every reader needs the pair, and a store nobody can read is a tax
//  the headless boards must not pay.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "Config.h"

namespace PeerPositions {

struct Position {
  double   latitude = 0.0, longitude = 0.0;
  float    altitudeM = 0.0f;
  float    accuracyM = 0.0f;
  uint32_t heardMs = 0;                  // our clock, when it arrived
};

#if HAS_LVGL_UI && HAS_GPS
void seen(const uint8_t hash[16], const Position& p);
bool get(const uint8_t hash[16], Position& out);
bool getByHex(const char* hashHex, Position& out);
size_t count();
#else
inline void seen(const uint8_t*, const Position&) {}
inline bool get(const uint8_t*, Position&) { return false; }
inline bool getByHex(const char*, Position&) { return false; }
inline size_t count() { return 0; }
#endif

} // namespace PeerPositions
