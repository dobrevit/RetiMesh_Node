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
//  VersionLabel.h — the firmware version as a narrow row can carry it
//
//  Since local builds started naming their commit, FW_VERSION is no longer a
//  word: it is `git describe` output — "v0.0.9-35-g8465afd", or with a
//  suffix when the tree was dirty. That is 18 characters against a status row
//  that has 21 columns and an address already in them, so the panel cut it in
//  the middle of the hash — which is the one place a cut destroys the value,
//  because half a hash identifies nothing.
//
//  So the row asks for what fits. All of it where there is room, which is the
//  e-paper and any release tag anywhere; the commit alone where there is not,
//  because that is the part that answers "which build is this?"; and nothing
//  at all where even that will not go, since a fragment is worse than an
//  honest blank.
//
//  Pure, and tested on the host: it is string arithmetic with several forms
//  to get right — tags, CI's dev-<sha>, describe output, dirty trees — and
//  none of them are worth discovering on a panel.
// ============================================================================
#pragma once

#include <stddef.h>
#include <string.h>

namespace VersionLabel {

// Writes into `out` the longest form of `version` that fits `room` columns,
// preferring the whole string, then the commit, then nothing. Always
// terminates. Returns the length written.
inline size_t fit(const char* version, size_t room, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!version || !*version || room == 0) return 0;

  const size_t whole = strlen(version);
  if (whole <= room && whole < cap) { memcpy(out, version, whole + 1); return whole; }

  // Not the trailing component: "-dirty" is the last one and says nothing
  // about which commit this is. The commit is what git marks with "-g", and
  // where there is no such mark — CI writes "dev-<sha>" — it is whatever
  // follows the last dash that is not the dirty flag.
  const bool dirty = strstr(version, "-dirty") != nullptr;
  const char* commit = strstr(version, "-g");
  if (commit) {
    commit += 1;                      // keep git's own "g" prefix on the hash
  } else {
    const char* dash = nullptr;
    for (const char* p = version; *p; p++)
      if (*p == '-' && strncmp(p, "-dirty", 6) != 0) dash = p;
    commit = dash ? dash + 1 : nullptr;
  }
  if (!commit || !*commit) return 0;

  // The hash ends where the next component begins.
  size_t len = 0;
  while (commit[len] && commit[len] != '-') len++;

  // A dirty tree is worth one character: which commit this came from is only
  // half the answer when the tree had uncommitted changes on top of it.
  const size_t want = len + (dirty ? 1 : 0);
  if (want > room || want >= cap) return 0;
  memcpy(out, commit, len);
  if (dirty) out[len] = '*';
  out[want] = '\0';
  return want;
}

} // namespace VersionLabel
