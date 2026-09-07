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
//  boot of an image that has already answered — and the case that matters is
//  worse than nothing: a solar node whose battery is flat at dawn brown-out
//  loops, and every cycle spends a transmission, and the transmission is one
//  of the larger loads on the supply that could not hold the last boot up.
//
//  So the answer is remembered rather than the question re-asked. What is
//  remembered is keyed to the image that answered it, because what the test
//  proves is the board header's pin map, and a different image can carry a
//  different one: an image that has not passed here proves itself however many
//  times its predecessor did.
//
//  A boot-reason check cannot do this job, which is why a stored marker exists
//  at all. The two things it would need — that a brown-out reads as a clean
//  boot, and that state kept in the RTC domain survives the rail dropping —
//  are exactly the two the failure breaks.
//
//  Pure, and byte-oriented rather than string-oriented, so the decision is a
//  host test rather than a bench session with a programmable supply: the
//  caller reads the marker out of NVS, hands in whatever bytes identify the
//  running image, asks here, and writes back what it is told to.
//
//  One rule runs through all three functions: an image that cannot be told
//  apart from another image is never proven. Bytes that identify nothing are
//  not folded into some marker of their own — they are the absent marker — so
//  a build that stopped carrying its identity costs one transmission a boot
//  rather than skipping the check on every board it is ever flashed to.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace RadioSelfTest {

// Nothing stored, and — the other half of the same idea — nothing identifiable
// to store. An image whose hash would land here is folded away from it, so "no
// marker" and "a marker" can never be read as each other; and an image that
// cannot say which image it is answers NO_MARK too, so it is never proven.
static const uint32_t NO_MARK = 0;

// The running image, as four bytes: FNV-1a over the bytes that identify it.
//
// The caller passes the ELF SHA-256 the application descriptor carries
// (`esp_app_get_description()->app_elf_sha256`), which is a property of the
// binary that is running: any edit that reaches the image changes it, and
// nothing else does.
//
// It is deliberately not FW_VERSION. That string is the tag for a CI build and
// `git describe --always --dirty` for a local one, and the dirty marker is a
// flag rather than a fingerprint — the value changes when HEAD moves or when
// the tree *first* becomes dirty, and then stays put however much more is
// edited (tools/fw_version.py says so in as many words). That is exactly the
// bring-up workflow this test exists for: edit the pin map, reflash, edit
// again. Keyed to the version, the second flash would find its predecessor's
// marker, skip the check and report the wiring proven on an image that had
// never once exercised it. FW_VERSION also falls back to "dev" wherever git
// cannot answer, which is one marker for every build ever made there.
//
// Four bytes rather than the digest itself so the marker is one fixed-width
// NVS entry and the comparison allocates nothing on a board that has none to
// spare. Two images collide once in 2^32; two versions collided by design.
//
// Bytes that carry no identity — no pointer, no length, or nothing but zeros —
// are NO_MARK rather than a hash of them. That is not tidiness: the digest
// reaches the caller from a field a build tool has to fill in, and a field
// nobody filled in is 32 zero bytes on every image ever built. Hashed, those
// give one constant, every image would share it, and the first board to pass
// would silence the check for all of them — on images that had never once
// driven the interrupt line. Answered as NO_MARK instead, proven() is false and
// markAfter() stores nothing, so an unidentifiable image runs the test every
// boot: the cost is one transmission, which is what the firmware did before any
// of this existed.
inline uint32_t buildMark(const void* bytes, size_t len) {
  const uint8_t* p = (const uint8_t*)bytes;
  if (p == nullptr || len == 0) return NO_MARK;
  uint32_t h = 2166136261UL;                   // FNV-1a offset basis
  bool identified = false;
  for (size_t i = 0; i < len; i++) {
    identified = identified || (p[i] != 0);
    h ^= (uint32_t)p[i];
    h *= 16777619UL;                           // FNV-1a prime
  }
  if (!identified) return NO_MARK;             // every image's "identity"
  return h == NO_MARK ? 1UL : h;
}

// Skip the self-test? Only when this exact image has already passed it. An
// absent marker, a marker from another image, and an image that cannot say
// which image it is are all the same answer: run.
inline bool proven(uint32_t stored, uint32_t image) {
  return stored != NO_MARK && stored == image;
}

// What to write back after a run. A failure stores nothing, so the next boot
// asks again: the whole value of the test is on a board whose interrupt pin is
// wrong, and a wrong pin that had been recorded as proven would be a node that
// never receives and never says why again.
inline uint32_t markAfter(bool passed, uint32_t image) {
  return passed ? image : NO_MARK;
}

} // namespace RadioSelfTest
