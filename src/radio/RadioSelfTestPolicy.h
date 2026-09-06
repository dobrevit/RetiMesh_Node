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
//  RadioSelfTestPolicy.h — when the boot self-test has already been paid for
//
//  The self-test transmits one short frame at boot to prove the interrupt line
//  is the pin the board header names (LoRaRadio::irqSelfTest). That is worth
//  its airtime on a board being brought up and worth nothing on the thousandth
//  boot of a build that has already answered — and the case that matters is
//  worse than nothing: a solar node whose battery is flat at dawn brown-out
//  loops, and every cycle spends a transmission, and the transmission is one
//  of the larger loads on the supply that could not hold the last boot up.
//
//  So the answer is remembered rather than the question re-asked. What is
//  remembered is keyed to the firmware that answered it, because what the test
//  proves is the board header's pin map, and a different image can carry a
//  different one: a build that has not passed here proves itself however many
//  times its predecessor did.
//
//  A boot-reason check cannot do this job, which is why a stored marker exists
//  at all. The two things it would need — that a brown-out reads as a clean
//  boot, and that state kept in the RTC domain survives the rail dropping —
//  are exactly the two the failure breaks.
//
//  Pure, so the decision is a host test rather than a bench session with a
//  programmable supply: the caller reads the marker out of NVS, asks here, and
//  writes back what it is told to.
// ============================================================================
#pragma once

#include <stdint.h>

namespace RadioSelfTest {

// Nothing stored. A build whose hash would land here is folded away from it,
// so "no marker" and "a marker" can never be read as each other.
static const uint32_t NO_MARK = 0;

// The running firmware, as four bytes: FNV-1a over FW_VERSION.
//
// That string is the tag CI builds from, or `git describe --always --dirty`
// for a local build (Config.h), so it moves with the commit and with an
// uncommitted edit — which is the granularity the pin-map question needs, and
// it is stable across every boot of one binary, which is what makes the marker
// worth keeping. Four bytes rather than the string itself so the marker is one
// fixed-width NVS entry and the comparison allocates nothing on a board that
// has none to spare.
inline uint32_t buildMark(const char* version) {
  uint32_t h = 2166136261UL;                   // FNV-1a offset basis
  for (const char* p = version; p && *p; p++) {
    h ^= (uint32_t)(uint8_t)*p;
    h *= 16777619UL;                           // FNV-1a prime
  }
  return h == NO_MARK ? 1UL : h;
}

// Skip the self-test? Only when this exact build has already passed it. An
// absent marker and a marker from another build are the same answer: run.
inline bool proven(uint32_t stored, uint32_t build) {
  return stored != NO_MARK && stored == build;
}

// What to write back after a run. A failure stores nothing, so the next boot
// asks again: the whole value of the test is on a board whose interrupt pin is
// wrong, and a wrong pin that had been recorded as proven would be a node that
// never receives and never says why again.
inline uint32_t markAfter(bool passed, uint32_t build) {
  return passed ? build : NO_MARK;
}

} // namespace RadioSelfTest
