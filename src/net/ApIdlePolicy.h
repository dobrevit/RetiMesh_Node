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
//  ApIdlePolicy.h — when an empty access point has earned its keep
//
//  An access point must beacon and cannot sleep, and on most nodes it beacons
//  for nobody: the portal is a maintenance surface, not a service. The rule
//  for when that stops being worth paying for lives here, once, and
//  WifiManager's tick asks.
//
//  The rule: while the feature is off, never. While anyone is associated, or
//  the access point is not actually up, the idle clock re-arms. Once the AP
//  has stood empty for the configured time, the verdict latches to
//  "suppress" — and stays latched, because the moment the AP goes down
//  nobody can associate to end the emptiness: the only ways back are a wake
//  (a button press, WIFI ON at the console, an admin message) or the feature
//  being switched off. A wake clears the latch at once and re-arms the
//  clock, so the AP that comes back gets its full idle window again.
//
//  Pure — no Arduino, no clock of its own, no Wi-Fi — so the decisions are
//  unit-tested on the host (test/test_ap_idle) rather than waited out on a
//  bench. Unsigned arithmetic makes the interval hold across a millis()
//  wrap; the latch itself holds no timestamp, so an AP that idles down for
//  months cannot wrap back up. One caller, one task: not synchronised, like
//  SampleGate and AutoIfPolicy.
// ============================================================================
#pragma once

#include <stdint.h>

class ApIdlePolicy {
public:
  // One verdict per ask: whether the access point should be held down.
  //  - enabled: the feature switch AND the AP's own link switch, combined by
  //    the caller — a policy for a link the operator turned off has nothing
  //    to decide, and saying so here is what clears the latch when either
  //    switch goes off (the AP then stays down by switch, not by policy).
  //  - apUp: the AP is actually on the air. The clock only counts an AP that
  //    is up and empty; down for any other reason (still starting, a scan
  //    holding convergence) re-arms it rather than counting toward a verdict
  //    about beacons that are not being sent.
  //  - stations: how many are associated right now. Any at all re-arms.
  //  - idleMs: the configured window (wifi.ap_idle_minutes, in ms). Passed
  //    per ask so a live settings change moves the very next decision.
  bool suppressed(uint32_t nowMs, bool enabled, bool apUp, uint8_t stations,
                  uint32_t idleMs) {
    if (!enabled) {
      // Off means never — and a latch left set here would bring the feature
      // back suppressed when the operator re-enables it, which is a node
      // that hides its AP the moment it is asked not to.
      _suppressed = false;
      _asked = false;              // the next enabled ask re-seeds the clock
      return false;
    }
    if (_suppressed) return true;  // beacon-less: only wake() or off ends it
    // The first enabled ask seeds the clock: a boot-time zero would read as
    // "empty since the epoch" and take the AP down on the spot, when the
    // window is supposed to start from the moment the counting does.
    if (!_asked || !apUp || stations > 0) {
      _asked = true;
      _emptySinceMs = nowMs;
      return false;
    }
    // Up and empty. The distance cannot outgrow the bound — the verdict
    // latches the instant it reaches it — so no wrap pinning is needed on
    // this side; the unsigned difference is wrap-correct until then.
    if (nowMs - _emptySinceMs >= idleMs) _suppressed = true;
    return _suppressed;
  }

  // A wake event: the operator (or their proxy — the button, the console,
  // an admin message) asked for the access point. Clears the latch at once
  // and re-arms the clock from now, so the returning AP gets its whole idle
  // window before the next verdict.
  void wake(uint32_t nowMs) {
    _suppressed = false;
    _asked = true;
    _emptySinceMs = nowMs;
  }

private:
  uint32_t _emptySinceMs = 0;
  bool     _suppressed   = false;
  bool     _asked        = false;
};
