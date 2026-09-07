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
//  BootRecord.h — what one run leaves behind for the next one to read
//
//  The layout that lives in RTC memory, and the two decisions made about it:
//  whether what is there came from a run of this firmware at all, and what
//  that run had to say. Both are here rather than in Diag.cpp because both
//  are pure arithmetic on a struct, and because getting either wrong is
//  silent — a record misread as valid reports a dead run's noise as fact, and
//  one misread as invalid throws away the only evidence a panic left.
//
//  Why the fault counts belong in here
//  -----------------------------------
//  They did not, and it cost six days. `Diag::faults()` reported
//  `alloc_failures` and `contained` out of ordinary DRAM statics, so every
//  restart zeroed them. Diag.h states the purpose those counters were added
//  for — a node under memory pressure should be visible as such *while it is
//  still running* — and for a running node they did exactly that. But the
//  question a soak actually asks is why a board that is no longer running
//  stopped, and a counter that dies with the run cannot answer it: a Heltec
//  Wireless Bridge panicked seven times between 2026-08-31 and 2026-09-07 and
//  every reading taken afterwards said `0, 0`, because every reading was
//  taken after the reboot that cleared it.
//
//  So they move in beside the run length, which has always survived this
//  boundary and is read the same way.
//
//  The magic, and what it is really deciding
//  -----------------------------------------
//  RTC memory holds across a panic, a watchdog reset and a software restart,
//  and does *not* hold across a power cut or a brownout. So the magic is not
//  a corruption check: it is the difference between "the firmware fell over"
//  and "the rail went away", which is evidence in its own right and is why a
//  missing record must read as unknown rather than as zero. It also catches
//  the other case that would otherwise be read as fact — a record written by
//  an older firmware with a different layout, whose bytes would land in the
//  wrong fields. The value changes whenever the struct does, which is why it
//  is versioned rather than arbitrary.
//
//  One upgrade cost, paid once: the first boot after the layout changes finds
//  the previous magic and reports the run before it as unknown. That is the
//  honest answer — that run's bytes are not in the fields this build reads.
// ============================================================================

#pragma once

#include <stdint.h>

namespace Diag {

// How a deliberate restart went, in milliseconds on the RTC clock: entering
// the restart, and handing over to the core's persist-restart (the composite
// USB device only, which is why persistMs is legitimately zero elsewhere).
// Zero in entryMs means no restart stamped these — the run ended some other
// way, which is the common case and not a fault.
struct RestartMarks { uint32_t entryMs, persistMs; };

// Everything one run leaves for the next. Laid out here, placed in RTC memory
// by Diag.cpp, and never read except through the functions below.
struct Record {
  uint32_t     magic;
  uint32_t     uptimeS;         // how long the run that wrote this had been up
  RestartMarks restart;
  // The fault counts as they stood when the run ended. Mirrored from the live
  // atomics rather than counted here: the authoritative counter is still the
  // atomic, because a count that only exists in RTC memory would be read and
  // written by every task that can fail an allocation.
  uint32_t     allocFailures;
  uint32_t     caught;
};

// "RTM3" — the record grew the fault counts. "RTM2" grew the restart marks;
// "RTM1" was the run length alone. Bump this whenever `Record` changes shape,
// and never reuse a value: an old record read through a new layout is the one
// failure this check exists to prevent.
constexpr uint32_t kRecordMagic = 0x52544D33;

// What the run that just ended reported. `known` false means the RTC domain
// did not hold — a power cut or a brownout — or the record was written by a
// firmware with a different layout. Either way the fields below are not
// evidence and must not be shown as zero.
struct Previous {
  bool     known         = false;
  uint32_t uptimeS       = 0;
  uint32_t allocFailures = 0;
  uint32_t caught        = 0;
  bool     restartMarked = false;
  RestartMarks restart   = {0, 0};
};

inline Previous readPrevious(const Record& r) {
  Previous p;
  if (r.magic != kRecordMagic) return p;
  p.known         = true;
  p.uptimeS       = r.uptimeS;
  p.allocFailures = r.allocFailures;
  p.caught        = r.caught;
  // A restart that did not stamp its entry left nothing to time. Reporting
  // that as a restart taking however long the RTC clock happens to read would
  // invent a measurement out of a zero.
  if (r.restart.entryMs) {
    p.restartMarked = true;
    p.restart       = r.restart;
  }
  return p;
}

// Claim the record for the run starting now. Prefer claimRun() below — this is
// the half that destroys the previous run's figures, and on its own it is only
// correct if the caller has already read them.
inline void beginRun(Record& r) {
  r.magic         = kRecordMagic;
  r.uptimeS       = 0;
  r.restart       = RestartMarks{0, 0};
  r.allocFailures = 0;
  r.caught        = 0;
}

// Take what the last run left, then claim the record for this one — in that
// order, in one call, because the order is the whole contract and splitting it
// across two statements in a caller is how it gets reversed.
//
// The risk is worth closing in the code rather than in a comment: Diag::begin()
// installs a new-handler that writes to this record, so between claiming and
// reading there is a window in which a failed allocation overwrites the dead
// run's count — the exact evidence the record exists to carry. Callers get one
// function and cannot sequence it wrongly.
inline Previous claimRun(Record& r) {
  const Previous previous = readPrevious(r);
  beginRun(r);
  return previous;
}

// How long the restart itself took, from the marks it left and the RTC clock
// now. Unsigned arithmetic throughout, deliberately: the RTC clock is a
// 32-bit millisecond count that wraps about every 49 days, and a node that
// restarts across the wrap must report the few milliseconds it actually took
// rather than a number near 2^32. Subtracting in uint32_t gives the right
// answer across the wrap; widening either side would not.
struct RestartTiming { uint32_t toPersistMs = 0, toBootMs = 0; bool known = false; };

inline RestartTiming restartTiming(const RestartMarks& m, uint32_t nowMs) {
  RestartTiming t;
  if (!m.entryMs) return t;
  t.known        = true;
  t.toPersistMs  = m.persistMs ? (uint32_t)(m.persistMs - m.entryMs) : 0u;
  t.toBootMs     = (uint32_t)(nowMs - (m.persistMs ? m.persistMs : m.entryMs));
  return t;
}

} // namespace Diag
