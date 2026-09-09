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
//  EnvPollPolicy.h — start a conversion, or collect one, but never wait for it
//
//  A BME280 in forced mode takes about ten milliseconds to convert, and the
//  loop that reads it must not spend them waiting. So a reading is two passes:
//  one that asks the part to convert, and a later one that collects the
//  answer. Which of those a pass is — or whether it is neither — is the whole
//  of this file.
//
//  It is here rather than inside Environment.cpp for the reason SampleGate and
//  GnssDutyPolicy are: the sequencing is arithmetic over two scalars, the
//  failure modes are silent, and a bench is a poor place to find them. A
//  converting flag that is never cleared is a sensor that reports the same
//  reading for ever; a collect that runs twice reads a conversion that has
//  already been consumed; a trigger that failed and was treated as started
//  waits ten milliseconds and then reads a register the part never filled.
//  None of those show up as a crash, and all three are decidable on a host.
//
//  The division of labour is SampleGate's: this says what to do, the caller
//  does it and says whether it worked. The policy never touches a bus and
//  never knows what a bus is.
//
//  Unsigned arithmetic throughout, so the conversion window holds across a
//  millis() wrap.
// ============================================================================
#pragma once

#include <stdint.h>

class EnvPollPolicy {
public:
  // What this pass of the loop should do.
  //
  //   Idle      nothing is due and nothing is in flight — the usual answer
  //   Trigger   ask the part to convert
  //   Wait      a conversion is in flight and has not had long enough
  //   Collect   it has, so read it out
  enum class Action : uint8_t { Idle, Trigger, Wait, Collect };

  explicit EnvPollPolicy(uint32_t convertMs) : _convertMs(convertMs) {}

  // `sampleDue` is the cadence's answer, which the caller owns — a SampleGate
  // in the driver's case. It is only consulted when nothing is in flight: a
  // conversion already started is finished regardless of what the gate thinks,
  // or the answer it produced would be thrown away.
  Action decide(uint32_t nowMs, bool sampleDue) const {
    if (!_converting) return sampleDue ? Action::Trigger : Action::Idle;
    return (uint32_t)(nowMs - _startedMs) < _convertMs ? Action::Wait
                                                       : Action::Collect;
  }

  // The caller did what Trigger said, and this is whether the part took it. A
  // refusal deliberately does *not* start the window: the gate has already
  // spent its turn, so the next attempt comes at the next interval rather than
  // on the next pass. Hammering a part that is not answering would cost the
  // bus it shares, and on this board it shares one with the panel.
  void triggered(uint32_t nowMs, bool ok) {
    _converting = ok;
    if (ok) _startedMs = nowMs;
  }

  // A conversion is no longer in flight, whether it was read out or abandoned
  // — a part still reporting itself busy after the window is abandoned, and
  // then waits its turn like any other reading. One call for both, because
  // nothing downstream treats them differently and two would be two states to
  // get wrong.
  void finished() { _converting = false; }

  // For the tests, and for a status line that wants to say what it is doing.
  bool converting() const { return _converting; }

private:
  uint32_t _convertMs;
  uint32_t _startedMs = 0;
  bool     _converting = false;
};
