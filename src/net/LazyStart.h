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
//  LazyStart.h — when a link that is built on demand may be built
//
//  PppUart and UsbNcm both allocate what their switch decides rather than
//  what the build decides, and both are driven from poll() on the loop task,
//  hundreds of times a second. That gives them the same two questions, and
//  the answers are not obvious enough to be written twice:
//
//    A build that failed must not be retried on the next pass. A node that
//    is out of byte-addressable RAM would otherwise bury the heap figures an
//    operator needs under thousands of copies of its own complaint about
//    them, down the port those figures have to arrive on. So a failure
//    latches, and the switch going off is what offers another try — which is
//    also the only gesture an operator has for "try again".
//
//    A teardown once begun runs to its end before anything is built again.
//    Otherwise a switch flicked off and straight back on leaves two
//    interfaces, or none, depending on the pass it lands in.
//
//  Both units had this; only one had it right. UsbNcm was written second and
//  reached review without the latch at all, which is the case for the rule
//  living here rather than in each of them.
// ============================================================================
#pragma once

#include <stdint.h>

namespace LocalLink {

class LazyStart {
public:
  // Whether to build now. `built` is the unit's own answer to "does the
  // thing exist" — a handle it holds — and `busy` is true while a teardown
  // is still running, which must finish first.
  bool shouldStart(bool enabled, bool built, bool busy) const {
    return enabled && !built && !busy && !_failed;
  }

  // Whether to begin giving it back now.
  bool shouldStop(bool enabled, bool built, bool busy) const {
    return !enabled && built && !busy;
  }

  // What a build attempt came to. A failure latches until the switch goes
  // off; a success clears whatever was there.
  void built(bool ok) { _failed = !ok; }

  // The switch is off and nothing is left to give back: the latch lifts, so
  // turning it on again is a fresh attempt rather than a refusal.
  void idle(bool enabled, bool built, bool busy) {
    if (!enabled && !built && !busy) _failed = false;
  }

  // Whether the last attempt failed, for a status line that would otherwise
  // report a link as merely "off" when it is off because it could not start.
  bool failed() const { return _failed; }

private:
  bool _failed = false;
};

} // namespace LocalLink
