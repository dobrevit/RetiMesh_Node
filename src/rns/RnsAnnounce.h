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

#include <string.h>

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

// Whether an announce for this aspect earns a row in the peers table.
//
// One RetiMesh node arrives here as several destinations: lxmf.delivery and
// nomadnetwork.node from this firmware, and retimesh.node as well from every
// node still running a build that announced it — three audiences, three
// app_data shapes, three hashes. A mesh of five nodes therefore fills the
// table with up to fifteen rows describing five things. Measured on a bench
// of six peers: sixteen rows, six names.
//
// lxmf.delivery is the address a person sends to, so it stays. retimesh.node
// stays too: this firmware no longer sends it (RnsTransport::loop), but the
// unattended half of a fleet does until it is updated, and it is the only
// announce that ever carried a firmware version — dropping it on receipt as
// well would make those neighbours vanish from the list whose job is to show
// what is out there. nomadnetwork.node carries a name a RetiMesh node already
// has from the other two and nothing else; its only purpose is letting a
// NomadNet client find a page to browse, which is a reason to keep
// *announcing* ours and no reason at all to remember another RetiMesh node's.
//
// The cost of doing it by aspect rather than by identity: a peer whose *only*
// announce is nomadnetwork.node — a plain `nomadnet --daemon` page host, not
// a RetiMesh node — has no other row to be found under, and so is not listed
// at all. The announce handler is handed the announcing identity; keying the
// rule on "this identity is already in the table" rather than on the aspect
// would keep such a host and still collapse a RetiMesh node to one row.
//
// Unknown aspects are kept. A name this table does not recognise is a peer
// running something we have not met, which is exactly the thing an operator
// wants to see in the list rather than the thing to hide from it.
inline bool worthRemembering(const char* aspect) {
  return !(aspect && strcmp(aspect, "nomadnetwork.node") == 0);
}

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

  // The delivery address, derived here rather than read back from the running
  // transport. Everything that tells an operator "this is the node, reach it
  // here" — the QR, the panel, the mDNS record, the status document — needs an
  // answer before Reticulum has started, and RnsTransport::lxmf().address is
  // empty until it has. Both are sha256(name_hash + identity_hash) over the
  // same identity and the same aspect, so they cannot disagree — and if they
  // ever did, /api/status carries both (`destination` from here,
  // `lxmf_address` from the transport) and the two would stop matching in
  // plain sight.
  const uint8_t* lxmfHash()   const { return _lxmfHash; }
  const char*    lxmfHex()    const { return _lxmfHex; }

private:
  uint8_t  _xPrv[32], _edSeed[32];
  uint8_t  _pub[Rns::KEY_LEN];
  uint8_t  _identityHash[Rns::HASH_LEN];
  uint8_t  _destHash[Rns::HASH_LEN];
  uint8_t  _lxmfHash[Rns::HASH_LEN];
  char     _destHex[2 * Rns::HASH_LEN + 1];
  char     _lxmfHex[2 * Rns::HASH_LEN + 1];
  char     _identityHex[2 * Rns::HASH_LEN + 1];
  uint32_t _emitted = 0;
  bool     _ok = false;
};

extern NodeIdentity nodeIdentity;
