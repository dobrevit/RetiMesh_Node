// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// PeerPositions.cpp — see PeerPositions.h.
#include "PeerPositions.h"

#if HAS_LVGL_UI

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>

namespace PeerPositions {
namespace {

struct Entry { uint8_t hash[16]; Position pos; bool used; };
constexpr size_t kMax = 24;
Entry sTable[kMax];
portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

} // namespace

void seen(const uint8_t hash[16], const Position& p) {
  taskENTER_CRITICAL(&sMux);
  Entry* slot = nullptr;
  uint32_t oldestAge = 0;
  const uint32_t now = p.heardMs;
  for (Entry& e : sTable) {
    if (e.used && memcmp(e.hash, hash, 16) == 0) { slot = &e; break; }
    const uint32_t age = e.used ? now - e.pos.heardMs : UINT32_MAX;
    if (!slot || age >= oldestAge) { oldestAge = age; slot = &e; }
  }
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
  if (!hashHex || strlen(hashHex) < 32) return false;
  uint8_t h[16];
  for (int i = 0; i < 16; i++) {
    unsigned v;
    if (sscanf(hashHex + i * 2, "%2x", &v) != 1) return false;
    h[i] = (uint8_t)v;
  }
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
#endif // HAS_LVGL_UI
