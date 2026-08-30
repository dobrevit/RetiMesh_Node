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
//  LxmfFormat.h — the LXMF wire format, both directions, and nothing else
//
//  Announcing an lxmf.delivery address is what makes a RetiMesh node visible
//  to the clients people actually use: Sideband and MeshChatX list LXMF and
//  NomadNet aspects and hide the rest, so a node that announced only
//  retimesh.node was in nobody's list however close it was.
//
//  Two directions of one format, kept in one file so they cannot drift: what
//  the node puts in its announce, and what it takes out of a message sent to
//  it. Pure — no crypto, no storage, no clock — because every byte it reads
//  arrives over the air from whoever is in earshot, and a parser for that
//  needs to be held to its cases without a radio. What it must never do is
//  read past the buffer it was handed; that is most of what its tests are
//  about, and why the length checks are written out rather than assumed.
//
//  Verifying a message is deliberately not here. It needs the sender's public
//  key, which needs an announce this node has heard, which is not a question
//  about bytes (RnsTransport::onLxmfPacket).
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Rns {

// An LXMF message as it arrives on the delivery destination, once the
// destination has decrypted it:
//
//   destination hash (16) | source hash (16) | signature (64) | payload
//
// and the payload is msgpack [timestamp, title, content, fields]. The
// signature is over destination hash + source hash + payload, so it binds the
// message to the pair it was sent between: a message replayed at another node
// does not verify, and neither does one whose text was altered.
struct LxmfMessage {
  const uint8_t* destHash;               // 16 bytes
  const uint8_t* sourceHash;             // 16
  const uint8_t* signature;              // 64
  const uint8_t* payload;                // msgpack, what the signature covers with the two hashes
  size_t         payloadLen;
  const uint8_t* title;   size_t titleLen;      // may be empty; LXMF allows it
  const uint8_t* content; size_t contentLen;
};

// One msgpack element: its value where it is a string or binary, and where it
// ends either way. Only the types LXMF puts in a message payload are
// understood — a timestamp, two texts and a map — and anything else is
// skipped by length rather than guessed at.
inline bool msgpackNext(const uint8_t* p, size_t n, size_t i,
                        const uint8_t*& val, size_t& valLen, size_t& next) {
  val = nullptr; valLen = 0;
  if (i >= n) return false;
  const uint8_t t = p[i];
  auto need = [&](size_t k) { return i + k <= n; };
  if ((t & 0xE0) == 0xA0) { valLen = t & 0x1F; val = p + i + 1; next = i + 1 + valLen; }        // fixstr
  else if (t == 0xD9 || t == 0xC4) {                                                            // str8 / bin8
    if (!need(2)) return false;
    valLen = p[i + 1]; val = p + i + 2; next = i + 2 + valLen;
  }
  else if (t == 0xDA || t == 0xC5) {                                                            // str16 / bin16
    if (!need(3)) return false;
    valLen = ((size_t)p[i + 1] << 8) | p[i + 2]; val = p + i + 3; next = i + 3 + valLen;
  }
  else if (t <= 0x7F || t >= 0xE0)     next = i + 1;                                            // fixint
  else if (t == 0xC0 || t == 0xC2 || t == 0xC3) next = i + 1;                                   // nil, false, true
  else if (t == 0xCC || t == 0xD0)     next = i + 2;
  else if (t == 0xCD || t == 0xD1)     next = i + 3;
  else if (t == 0xCE || t == 0xD2 || t == 0xCA) next = i + 5;                                   // u32/i32/float32
  else if (t == 0xCF || t == 0xD3 || t == 0xCB) next = i + 9;                                   // u64/i64/float64
  else if ((t & 0xF0) == 0x80 || (t & 0xF0) == 0x90) next = i + 1;                              // fixmap/fixarray header
  else return false;                                                                            // not a shape LXMF uses
  return next <= n;
}

inline size_t lxmfAppData(const char* name, uint8_t stampCost, uint8_t* out, size_t cap) {
  const size_t n = name ? strlen(name) : 0;
  // fixstr carries up to 31 bytes and is what every LXMF client emits for a
  // node name; str8 covers the rest. Nothing here needs more than 255.
  const bool fixstr = n <= 31;
  const size_t need = 1 + (fixstr ? 1 : 2) + n + 1;      // array, str header, name, cost
  if (n > 255 || need > cap) return 0;
  uint8_t* p = out;
  *p++ = 0x92;                                           // fixarray of 2
  if (fixstr) *p++ = (uint8_t)(0xA0 | n);
  else      { *p++ = 0xD9; *p++ = (uint8_t)n; }
  memcpy(p, name, n); p += n;
  // A positive fixint; stamp costs above 127 are not a thing this emits.
  *p++ = (uint8_t)(stampCost & 0x7F);
  return (size_t)(p - out);
}

// The name out of an LXMF announce's app_data — the other direction of
// lxmfAppData(), and here beside it for the same reason. Returns bytes
// written; 0 when the app_data is not an LXMF array, which leaves the caller
// to treat it as whatever else it might be.
inline size_t lxmfName(const uint8_t* app, size_t len, char* out, size_t cap) {
  if (!app || len < 2 || cap < 2) return 0;
  if ((app[0] & 0xF0) != 0x90) return 0;                 // not the array LXMF sends
  const uint8_t* v = nullptr; size_t vl = 0, next = 0;
  if (!msgpackNext(app, len, 1, v, vl, next) || !v || vl == 0) return 0;
  const size_t k = vl < cap - 1 ? vl : cap - 1;
  memcpy(out, v, k); out[k] = '\0';
  return k;
}

inline bool parseLxmf(const uint8_t* data, size_t len, LxmfMessage& out) {
  const size_t kHash = 16, kSig = 64;
  if (!data || len < kHash + kHash + kSig + 1) return false;
  out = LxmfMessage{};
  out.destHash   = data;
  out.sourceHash = data + kHash;
  out.signature  = data + kHash * 2;
  out.payload    = data + kHash * 2 + kSig;
  out.payloadLen = len - (kHash * 2 + kSig);

  const uint8_t* p = out.payload; const size_t n = out.payloadLen;
  if (n < 1 || (p[0] & 0xF0) != 0x90) return false;      // fixarray [timestamp, title, content, fields]
  if ((p[0] & 0x0F) < 3) return false;                   // the three we read
  size_t i = 1, next = 0; const uint8_t* v = nullptr; size_t vl = 0;
  if (!msgpackNext(p, n, i, v, vl, next)) return false;  // timestamp, skipped
  i = next;
  if (!msgpackNext(p, n, i, v, vl, next)) return false;  // title
  out.title = v; out.titleLen = vl; i = next;
  if (!msgpackNext(p, n, i, v, vl, next)) return false;  // content
  out.content = v; out.contentLen = vl;
  return true;
}

} // namespace Rns
