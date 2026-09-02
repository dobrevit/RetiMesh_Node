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
//  Neighbors.h — table of stations heard on the mesh
//
//  Fed from three sources (see LoRaRadio.h / RnsAnnounce.h):
//    * Reticulum announces (LoRa or Wi-Fi clients) — verified, with
//      destination hash, aspect, hops and a best-effort display name
//    * RetiMesh beacons (retimesh.beacon PLAIN broadcasts)
//    * RNode station IDs (raw callsigns)
//  Read by the web status page and the OLED. Fixed table, oldest evicted.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "Config.h"

enum class NeighborKind : uint8_t { StationId = 0, Beacon = 1, Announce = 2 };

struct Neighbor {
  char         name[33];                 // display name / callsign (may be empty)
  char         version[16];
  char         hash[33];                 // hex destination hash (announces)
  char         aspect[24];               // "lxmf.delivery", ... or "" if unknown
  float        rssi;
  float        snr;
  uint32_t     lastSeen;                 // millis()
  uint32_t     count;                    // how many times heard
  uint8_t      hops;
  bool         viaWifi;                  // heard from a TCP client, not RF
  NeighborKind kind;
  bool         used;
};

class Neighbors {
public:
  // Records a sighting. Keyed by hash when set, else by name.
  void   seen(const Neighbor& info);
  // Copies up to `max` entries into `out`, newest first. Returns the count.
  size_t snapshot(Neighbor* out, size_t max);
  // One neighbor by its hex destination hash — how the mesh screens put a
  // name and a signal to a path-table entry. False when never heard.
  bool   byHash(const char* hashHex, Neighbor& out);
  size_t count(uint32_t maxAgeMs);       // heard within maxAgeMs

private:
  Neighbor     _n[MAX_NEIGHBORS] = {};
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

extern Neighbors neighbors;
