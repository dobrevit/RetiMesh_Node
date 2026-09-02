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

void Neighbors::seen(const Neighbor& info) {
  uint32_t now = millis();
  bool byHash = info.hash[0] != '\0';
  portENTER_CRITICAL(&_mux);
  Neighbor* slot = nullptr;
  for (Neighbor& n : _n) {
    if (!n.used) continue;
    if (byHash ? strcmp(n.hash, info.hash) == 0 : (n.hash[0] == '\0' && strcmp(n.name, info.name) == 0)) { slot = &n; break; }
  }
  uint32_t prevCount = 0;
  if (slot) prevCount = slot->count;
  else {                                  // new: free slot, else the oldest
    for (Neighbor& n : _n) if (!n.used) { slot = &n; break; }
    if (!slot) {
      slot = &_n[0];
      for (Neighbor& n : _n) if ((int32_t)(n.lastSeen - slot->lastSeen) < 0) slot = &n;
    }
  }
  *slot = info;
  slot->used = true;
  slot->lastSeen = now;
  slot->count = prevCount + 1;
  portEXIT_CRITICAL(&_mux);
}

size_t Neighbors::snapshot(Neighbor* out, size_t max) {
  size_t k = 0;
  portENTER_CRITICAL(&_mux);
  for (const Neighbor& n : _n) if (n.used && k < max) out[k++] = n;
  portEXIT_CRITICAL(&_mux);
  for (size_t i = 1; i < k; i++) {        // newest first
    Neighbor t = out[i]; size_t j = i;
    while (j > 0 && (int32_t)(out[j - 1].lastSeen - t.lastSeen) < 0) { out[j] = out[j - 1]; j--; }
    out[j] = t;
  }
  return k;
}

bool Neighbors::byHash(const char* hashHex, Neighbor& out) {
  bool found = false;
  portENTER_CRITICAL(&_mux);
  for (const Neighbor& n : _n)
    if (n.used && strcmp(n.hash, hashHex) == 0) { out = n; found = true; break; }
  portEXIT_CRITICAL(&_mux);
  return found;
}

size_t Neighbors::count(uint32_t maxAgeMs) {
  size_t c = 0; uint32_t now = millis();
  portENTER_CRITICAL(&_mux);
  for (const Neighbor& n : _n) if (n.used && now - n.lastSeen <= maxAgeMs) c++;
  portEXIT_CRITICAL(&_mux);
  return c;
}
