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

} // namespace Rns

// ---------------------------------------------------------------------------
// This node's Reticulum identity and its "retimesh.node" destination.
// ---------------------------------------------------------------------------
class NodeIdentity {
public:
  // Loads the keys from NVS (namespace retimeshid) or generates them once.
  bool begin();

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
