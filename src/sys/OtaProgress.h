// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  OtaProgress.h — what the installer is doing, for whoever is watching
//
//  A single-writer progress line: the HTTP task installing writes it, the
//  display task reads it. Plain fields on purpose — a torn read costs one
//  frame of a progress bar, and a lock here would be the only lock the
//  host-tested installer ever needed.
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

namespace OtaProgress {

struct State {
  bool     active = false;
  bool     failed = false;               // meaningful on the active->idle edge
  uint32_t written = 0;
  uint32_t total = 0;
  char     stage[16] = {0};
};

inline State sState;

inline void begin(uint32_t total) {
  sState.written = 0;
  sState.total = total;
  strncpy(sState.stage, "writing", sizeof(sState.stage) - 1);
  sState.failed = false;
  sState.active = true;
}
inline void step(uint32_t written) { sState.written = written; }
inline void phase(const char* s) {
  strncpy(sState.stage, s, sizeof(sState.stage) - 1);
  sState.stage[sizeof(sState.stage) - 1] = 0;
}
inline void end(bool ok) {
  if (!sState.active) return;
  sState.failed = !ok;
  sState.active = false;
}
inline State get() { return sState; }

} // namespace OtaProgress
