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
//  Neighbors.h — table of stations heard on the LoRa channel
//
//  Fed by the radio task from beacon frames (see LoRaRadio.h: RetiMesh
//  "RM1" beacons and plain RNode station-ID callsigns), read by the web
//  status page and the OLED. Small fixed table, oldest entry evicted.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "Config.h"

enum class NeighborKind : uint8_t { StationId = 0, RetiMesh = 1 };

struct Neighbor {
  char         name[33];
  char         version[16];
  float        rssi;
  float        snr;
  uint32_t     lastSeen;                 // millis()
  uint32_t     beacons;                  // how many heard
  NeighborKind kind;
  bool         used;
};

class Neighbors {
public:
  void   seen(const char* name, const char* version, NeighborKind kind, float rssi, float snr);
  // Copies up to `max` entries into `out`, newest first. Returns the count.
  size_t snapshot(Neighbor* out, size_t max);
  size_t count(uint32_t maxAgeMs);       // heard within maxAgeMs

private:
  Neighbor     _n[MAX_NEIGHBORS] = {};
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern Neighbors neighbors;
