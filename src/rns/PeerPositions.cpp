// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// PeerPositions.cpp — see PeerPositions.h.
#include "PeerPositions.h"

#if HAS_LVGL_UI && HAS_GPS

#include <string.h>
#include <freertos/FreeRTOS.h>
#include "RnsAnnounce.h"

namespace PeerPositions {
namespace {

struct Entry { uint8_t hash[16]; Position pos; bool used; };
constexpr size_t kMax = 24;
Entry sTable[kMax];
// A spinlock, consciously: the holds are a short scan-and-copy, no ISR ever
// touches the table, and Neighbors — the closest sibling — made the same
// call. If kMax grows past a few dozen, revisit with Sys::Lock.
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

} // namespace

void seen(const uint8_t hash[16], const Position& p) {
  taskENTER_CRITICAL(&sMux);
  Entry* slot = &sTable[0];
  uint32_t oldestAge = 0;
  bool matched = false;
  for (Entry& e : sTable) {
    if (e.used && memcmp(e.hash, hash, 16) == 0) { slot = &e; matched = true; break; }
    // Age, never the raw stamp: the wrap-safe form this codebase settled on.
    const uint32_t age = e.used ? p.heardMs - e.pos.heardMs : UINT32_MAX;
    if (age >= oldestAge) { oldestAge = age; slot = &e; }
  }
  (void)matched;
  memcpy(slot->hash, hash, 16);
  slot->pos = p;
  slot->used = true;
  taskEXIT_CRITICAL(&sMux);
}

bool get(const uint8_t hash[16], Position& out) {
  bool found = false;
  taskENTER_CRITICAL(&sMux);
  for (const Entry& e : sTable)
    if (e.used && memcmp(e.hash, hash, 16) == 0) { out = e.pos; found = true; break; }
  taskEXIT_CRITICAL(&sMux);
  return found;
}

bool getByHex(const char* hashHex, Position& out) {
  uint8_t h[16];
  if (!Rns::hexToBytes16(hashHex, h)) return false;
  return get(h, out);
}

size_t count() {
  size_t c = 0;
  taskENTER_CRITICAL(&sMux);
  for (const Entry& e : sTable) if (e.used) c++;
  taskEXIT_CRITICAL(&sMux);
  return c;
}

} // namespace PeerPositions
#endif // HAS_LVGL_UI && HAS_GPS
