// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  PeerNames.h — names, remembered
//
//  The live neighbor table forgets at reboot, and a phone that talks over an
//  established link may not announce again for hours — so the glass showed
//  hashes for senders the node knew perfectly well yesterday. This is the
//  memory: every named announce is written down, and a name once heard is a
//  name kept. Only boards with a glass compile the real thing — headless
//  nodes must not pay flash cycles for labels they cannot draw.
// ============================================================================
#pragma once

#include <stddef.h>
#include "Config.h"

#if HAS_LVGL_UI
namespace PeerNames {
void remember(const char* hashHex, const char* name);   // no-op when unchanged
bool lookup(const char* hashHex, char* out, size_t n);  // false when never heard
void wipe();                                            // the erase control asks
} // namespace PeerNames
#else
namespace PeerNames {
inline void remember(const char*, const char*) {}
inline bool lookup(const char*, char*, size_t) { return false; }
inline void wipe() {}
} // namespace PeerNames
#endif
