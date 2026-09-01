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
//  OtaFloor.h — how far back an update may take this node
//
//  There is no clock on this hardware that survives a flat battery, so an
//  update cannot be judged by when it was signed. It is judged by a number that
//  only goes up: the node records the highest secure_version it has accepted
//  and refuses anything below it. A leaked signing key can still sign *a* new
//  image; it cannot push a node back onto a generation that has been retired.
//
//  The decision that matters is *when* the number moves, and it is not when the
//  image is written. An image that is written but never runs must leave the
//  floor alone — otherwise a node that wrote a bad update, failed to boot it
//  and rolled back would be sitting at a floor above the firmware it is running
//  and would refuse the corrected build carrying the same version. So an
//  install stages its version, and the boot that follows decides:
//
//    wrote it, booted it   -> the floor moves up to it
//    wrote it, rolled back -> the floor stays, the staged record is dropped
//
//  settle() is that decision with nothing else attached — no NVS, no
//  partitions — because it is the part worth testing and neither of those can
//  be tested on a host. Floor is the thin part that remembers the answer.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Ota {

// What a boot concluded about the floor.
struct Settlement {
  uint32_t floor;         // what the floor is now
  bool     clearStaged;   // the staged record has served its purpose
  bool     advanced;      // the floor moved (worth a log line; nothing else)
};

// `stagedIsRunning` answers "is the image that was staged the one that came
// up?". A node that rolled back answers no, and the version it wrote never
// earns the floor.
//
// The floor never decreases: a staged version below it settles to no change.
// That is not a redundant guard — an operator re-signing an older build with a
// lower secure_version is exactly the mistake the floor exists to survive, and
// it must not undo the floor just because the image happened to boot.
constexpr Settlement settle(uint32_t stored, bool haveStaged, uint32_t staged,
                            bool stagedIsRunning) {
  if (!haveStaged)      return {stored, false, false};
  if (!stagedIsRunning) return {stored, true,  false};
  if (staged <= stored) return {stored, true,  false};
  return {staged, true, true};
}

// The floor as it is remembered between boots. Templated on the store so the
// sequence — read, settle, write, drop the staged record — is exercised by
// tests against a fake, rather than only ever running on a device where a wrong
// order would show up as a node that will not take its next update.
//
// A store provides: bool has(key), uint32_t get(key, fallback),
// void put(key, value), void drop(key).
template <typename Store>
class Floor {
 public:
  static constexpr const char* ACCEPTED_KEY = "ota_floor";
  static constexpr const char* STAGED_KEY   = "ota_staged";

  explicit Floor(Store& store) : _store(store) {}

  // What FirmwareManifest::Policy.acceptedVersion should carry.
  uint32_t accepted() const { return _store.get(ACCEPTED_KEY, 0); }

  // Called once an image has been written and otadata switched to it. Recorded
  // before the reboot because after it there is no one left to write it down.
  void stage(uint32_t version) { _store.put(STAGED_KEY, version); }

  bool haveStaged() const { return _store.has(STAGED_KEY); }

  // Called on the boot after an install, with the answer to "did the image we
  // staged actually come up?".
  Settlement confirm(bool stagedIsRunning) {
    const Settlement s = settle(accepted(), haveStaged(),
                                _store.get(STAGED_KEY, 0), stagedIsRunning);
    // The floor first: if power is lost between these two writes, a staged
    // record that outlives its floor is harmless — the next boot settles it
    // again to the same answer — whereas a floor that never got written while
    // the record was dropped would silently lose the advance.
    if (s.advanced) _store.put(ACCEPTED_KEY, s.floor);
    if (s.clearStaged) _store.drop(STAGED_KEY);
    return s;
  }

 private:
  Store& _store;
};

}  // namespace Ota
