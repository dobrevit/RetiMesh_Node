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
//  Mdns.h — turning a node's name into something DNS will accept
//
//  Kept separate from WifiManager for one reason: it is the only part of the
//  naming that is pure, so it is the only part that can be tested without a
//  radio, a filesystem and a web server. A name that comes out wrong here is a
//  node nobody can reach, and that deserves better than being verified by
//  flashing a board and typing a URL.
// ============================================================================

#pragma once

#include <ctype.h>
#include <stddef.h>

namespace Mdns {

// Reduce `in` to a legal single DNS label in `out`.
//
// Labels are letters, digits and hyphens, compared without regard to case, and
// may not begin or end with a hyphen. Everything else in the source name — the
// spaces and punctuation an operator may well have put in an access-point name
// — collapses to a single hyphen, so "Shed roof #2" becomes "shed-roof-2".
//
// `fallback` is used when nothing usable survives, which would otherwise leave
// the node with no name to register at all.
inline void label(const char* in, char* out, size_t len, const char* fallback) {
  size_t o = 0;
  if (len == 0) return;
  for (const char* p = in; p && *p && o + 1 < len; p++) {
    const unsigned char c = (unsigned char)*p;
    if (isalnum(c))                            out[o++] = (char)tolower(c);
    else if (o > 0 && out[o - 1] != '-')       out[o++] = '-';
  }
  while (o > 0 && out[o - 1] == '-') o--;      // never trailing
  out[o] = '\0';

  if (o == 0 && fallback) {
    size_t f = 0;
    while (fallback[f] && f + 1 < len) { out[f] = fallback[f]; f++; }
    out[f] = '\0';
  }
}

} // namespace Mdns
