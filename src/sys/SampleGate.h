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
//  SampleGate.h — whether a periodic sample is due, worked out once
//
//  Three battery samplers each rationed their hardware reads the same way —
//  "not more than once per BATTERY_SAMPLE_MS" — and each solved the first-call
//  problem differently: one primed itself in begin(), one carried an
//  `everRead` flag, and one forgot about it and answered from zero-initialised
//  state for the first ten seconds of every boot. The rule lives here instead,
//  and the first ask is always due: a gate that starts out "recently sampled"
//  holds back exactly the reading everything at boot is waiting on.
//
//  Pure — no Arduino, no clock of its own — so the decisions are unit-tested
//  on the host (test/test_sample_gate) rather than waited out on a bench.
//  Unsigned arithmetic makes the interval hold across a millis() wrap.
//
//  Not synchronised. Callers that can race hold their own lock or tolerate a
//  doubled sample, exactly as the per-site timestamps they replaced did.
// ============================================================================
#pragma once

#include <stdint.h>

class SampleGate {
public:
  explicit SampleGate(uint32_t intervalMs) : _intervalMs(intervalMs) {}

  // True when a sample is due: on the very first ask (whatever `nowMs` is,
  // including 0), and then once the interval has passed since the last due
  // ask. Answering "due" starts the next interval — the attempt is what is
  // being rationed, so a read that then fails still waits its turn, keeping
  // whatever answer stood before.
  bool due(uint32_t nowMs) {
    if (_asked && (uint32_t)(nowMs - _lastMs) < _intervalMs) return false;
    _asked = true;
    _lastMs = nowMs;
    return true;
  }

  // A sample was just taken outside the gate's own cadence — begin() reading
  // the hardware before any task can ask. The gate then holds its full
  // interval from here rather than treating the next ask as the first.
  void prime(uint32_t nowMs) {
    _asked = true;
    _lastMs = nowMs;
  }

private:
  uint32_t _intervalMs;
  uint32_t _lastMs = 0;
  bool     _asked  = false;
};
