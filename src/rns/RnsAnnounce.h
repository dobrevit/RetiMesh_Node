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
//  RnsAnnounce.h — Reticulum announces: parse/verify incoming ones, and
//  produce our own from a persistent node identity.
//
//  Wire format (from RNS.Destination.announce / Identity.validate_announce):
//
//    packet  = flags(1) hops(1) [transport_id(16)] dest_hash(16) context(1) data
//    data    = public_key(64) name_hash(10) random_hash(10) [ratchet(32)]
//              signature(64) app_data
//    signed  = dest_hash + public_key + name_hash + random_hash + [ratchet]
//              + app_data                      (Ed25519, key = public_key[32:])
//    checks  = dest_hash == sha256(name_hash + sha256(public_key)[:16])[:16]
//
//  public_key = X25519 pub (32) + Ed25519 pub (32); random_hash = 5 random
//  bytes + 5-byte big-endian emission time (RNS treats a larger value as a
//  newer announce for the same destination).
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace Rns {

// ---------------------------------------------------------------------------
// When to announce next
// ---------------------------------------------------------------------------
// Pure arithmetic, out here where the native tests can reach it. Inside the
// task loop none of this was exercised: not the wrap, not the jitter bound,
// not the promotion of a uint16 interval into milliseconds.
//
// Scattered, not on the dot. Two nodes flashed together boot together and
// would then announce together for as long as they both run — the interval is
// fixed and nothing else moves the phase, so their packets collide every time
// and one of them is never heard.
//
// Scattered *around* the interval, not after it. Adding jitter and never
// subtracting it made the configured value a floor the node never met: every
// announce came late by somewhere between nothing and a tenth of the interval,
// so a node set to announce every minute managed about 1371 a day against the
// 1440 asked for — and at the 12-hour maximum the settings allow, an announce
// booked for noon could arrive as late as 13:12 and never before noon. The
// spread is the same width; it is centred now, so the configured interval is
// what the node averages.
//
// `rnd` is any random value; the caller passes esp_random(). Returns an
// absolute millis() deadline.
inline uint32_t nextAnnounceAt(uint16_t intervalS, uint32_t nowMs, uint32_t rnd) {
  if (!intervalS) return nowMs;                     // caller checks this; defined anyway
  const uint32_t base   = (uint32_t)intervalS * 1000UL;
  const uint32_t spread = base / 10;                // a tenth, as before
  return nowMs + base - spread / 2 + (spread ? rnd % (spread + 1) : 0);
}

// Bring a booked announce forward when the interval is lowered.
//
// Nothing on the settings path touches the schedule, so a node running at the
// 12-hour maximum that is dropped to a minute — because neighbours cannot see
// it, which is when an operator does this — went on saying nothing for up to
// 13 hours while the status page reported the new interval. A deadline further
// out than the new interval allows is pulled in to it.
inline uint32_t clampAnnounceTo(uint32_t bookedMs, uint16_t intervalS, uint32_t nowMs) {
  const uint32_t latest = nowMs + (uint32_t)intervalS * 1000UL;
  return (int32_t)(bookedMs - latest) > 0 ? latest : bookedMs;
}


constexpr size_t HASH_LEN   = 16;
constexpr size_t NAME_HASH  = 10;
constexpr size_t KEY_LEN    = 64;
constexpr size_t SIG_LEN    = 64;
constexpr size_t RANDOM_LEN = 10;
constexpr size_t RATCHET    = 32;
constexpr uint8_t PT_ANNOUNCE = 0x01;

struct Announce {
  const uint8_t* destHash;
  const uint8_t* nameHash;
  const uint8_t* publicKey;
  const uint8_t* appData;
  size_t         appDataLen;
  uint8_t        identityHash[HASH_LEN];
  uint8_t        hops;
  bool           hasRatchet;
};

// True if raw is a Reticulum ANNOUNCE packet (structurally).
bool isAnnounce(const uint8_t* raw, size_t len);

// Parses and fully validates (structure, destination hash, Ed25519
// signature). Pointers in `out` reference `raw`.
bool parseAnnounce(const uint8_t* raw, size_t len, Announce& out);

// Human name for a known name hash ("lxmf.delivery", ...), else nullptr.
const char* aspectName(const uint8_t nameHash[NAME_HASH]);

// Best-effort display name from app_data (plain text, or the first element
// of an LXMF-style msgpack array). Returns bytes written (0 = none).
size_t displayName(const Announce& a, char* out, size_t cap);

// The LXMF wire format both directions live in LxmfFormat.h, which is pure
// and tested on the host; this file needs it for displayName().

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void toHex(const uint8_t* data, size_t len, char* out);   // out needs 2*len+1
bool hexToBytes16(const char* hex, uint8_t out[16]);      // 32 lowercase/upper hex digits

} // namespace Rns

// ---------------------------------------------------------------------------
// This node's Reticulum identity and its "retimesh.node" destination.
// ---------------------------------------------------------------------------
class NodeIdentity {
public:
  // Loads the keys from NVS (namespace retimeshid) or generates them once.
  bool begin();

  // The other half of begin(): forgets the stored keys, so the next boot
  // invents a stranger. Touches nothing else — the caller decides what
  // else dies with the persona.
  void destroy();

  const uint8_t* publicKey()  const { return _pub; }        // 64 bytes
  // RNS private key layout: X25519 private (32) + Ed25519 seed (32).
  size_t privateKey(uint8_t out[64]) const { memcpy(out, _xPrv, 32); memcpy(out + 32, _edSeed, 32); return 64; }
  const uint8_t* identityHash() const { return _identityHash; }
  const uint8_t* destHash()   const { return _destHash; }
  const char*    destHex()    const { return _destHex; }
  const char*    identityHex() const { return _identityHex; }

  // Builds a complete announce packet with the given app_data into `out`.
  // Returns the packet length, 0 on error. Bumps the persisted emission
  // counter.
  size_t buildAnnounce(const uint8_t* appData, size_t appLen, uint8_t* out, size_t cap);

private:
  uint8_t  _xPrv[32], _edSeed[32];
  uint8_t  _pub[Rns::KEY_LEN];
  uint8_t  _identityHash[Rns::HASH_LEN];
  uint8_t  _destHash[Rns::HASH_LEN];
  char     _destHex[2 * Rns::HASH_LEN + 1];
  char     _identityHex[2 * Rns::HASH_LEN + 1];
  uint32_t _emitted = 0;
  bool     _ok = false;
};

extern NodeIdentity nodeIdentity;
