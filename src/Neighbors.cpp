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
//  Neighbors.cpp — see Neighbors.h
// ============================================================================
#include "Neighbors.h"

Neighbors neighbors;

void Neighbors::seen(const char* name, const char* version, NeighborKind kind, float rssi, float snr) {
  uint32_t now = millis();
  portENTER_CRITICAL(&_mux);
  Neighbor* slot = nullptr;
  for (Neighbor& n : _n) {
    if (n.used && strcmp(n.name, name) == 0) { slot = &n; break; }
  }
  if (!slot) {                            // new: take a free slot, else the oldest
    for (Neighbor& n : _n) if (!n.used) { slot = &n; break; }
    if (!slot) {
      slot = &_n[0];
      for (Neighbor& n : _n) if ((int32_t)(n.lastSeen - slot->lastSeen) < 0) slot = &n;
    }
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->used = true;
  }
  strlcpy(slot->version, version ? version : "", sizeof(slot->version));
  slot->kind     = kind;
  slot->rssi     = rssi;
  slot->snr      = snr;
  slot->lastSeen = now;
  slot->beacons++;
  portEXIT_CRITICAL(&_mux);
}

size_t Neighbors::snapshot(Neighbor* out, size_t max) {
  size_t k = 0;
  portENTER_CRITICAL(&_mux);
  for (const Neighbor& n : _n) if (n.used && k < max) out[k++] = n;
  portEXIT_CRITICAL(&_mux);
  // newest first (tiny array — insertion sort is fine)
  for (size_t i = 1; i < k; i++) {
    Neighbor t = out[i]; size_t j = i;
    while (j > 0 && (int32_t)(out[j - 1].lastSeen - t.lastSeen) < 0) { out[j] = out[j - 1]; j--; }
    out[j] = t;
  }
  return k;
}

size_t Neighbors::count(uint32_t maxAgeMs) {
  size_t c = 0; uint32_t now = millis();
  portENTER_CRITICAL(&_mux);
  for (const Neighbor& n : _n) if (n.used && now - n.lastSeen <= maxAgeMs) c++;
  portEXIT_CRITICAL(&_mux);
  return c;
}
