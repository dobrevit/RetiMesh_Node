// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  PeerNames.h — names, remembered
//
//  The live neighbor table forgets at reboot, and a phone that talks over an
//  established link may not announce again for hours — so the glass showed
//  hashes for senders it knew perfectly well yesterday. This is the memory:
//  every named announce is written down, and a name once heard is a name
//  kept. RAM-backed with a small file behind it; the file is rewritten only
//  when a name actually changes.
// ============================================================================
#pragma once

#include <stddef.h>

namespace PeerNames {

void remember(const char* hashHex, const char* name);   // no-op when unchanged
bool lookup(const char* hashHex, char* out, size_t n);  // false when never heard

} // namespace PeerNames
