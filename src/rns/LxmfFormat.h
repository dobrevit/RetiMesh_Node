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
// and the payload is msgpack [timestamp, title, content, fields].
//
// What the signature covers is not those three but those three *and their own
// hash*: hashed_part = dest || source || payload, and the signed data is
// hashed_part || SHA256(hashed_part). Getting that wrong does not fail
// safely — it refuses every message a real client sent, as a forgery, which
// accuses the honest sender. It binds the message to the pair of addresses
// either way: replayed at another node it does not verify, and neither does
// an altered text.
struct LxmfMessage {
  const uint8_t* destHash;               // 16 bytes
  const uint8_t* sourceHash;             // 16
  const uint8_t* signature;              // 64
  const uint8_t* payload;                // msgpack, what the signature covers with the two hashes
  size_t         payloadLen;
  const uint8_t* title;   size_t titleLen;      // may be empty; LXMF allows it
  const uint8_t* content; size_t contentLen;
  double         sentAt;                        // the sender's clock, seconds since the epoch; 0 if absent
};

// One msgpack element: its value where it is a string or binary, and where it
// ends either way. Only the types LXMF puts in a message payload are
// understood — a timestamp, two texts and a map — and anything else is
// skipped by length rather than guessed at.
inline bool msgpackNext(const uint8_t* p, size_t n, size_t i,
                        const uint8_t*& val, size_t& valLen, size_t& next,
                        unsigned depth = 8) {
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
  else if (t == 0xDB || t == 0xC6) {                                                            // str32 / bin32
    if (!need(5)) return false;
    valLen = ((size_t)p[i + 1] << 24) | ((size_t)p[i + 2] << 16) |
             ((size_t)p[i + 3] << 8) | p[i + 4];
    val = p + i + 5; next = i + 5 + valLen;
    if (next < i) return false;                                                                 // length that wrapped
  }
  else if (t <= 0x7F || t >= 0xE0)     next = i + 1;                                            // fixint
  else if (t == 0xC0 || t == 0xC2 || t == 0xC3) next = i + 1;                                   // nil, false, true
  else if (t == 0xCC || t == 0xD0)     next = i + 2;
  else if (t == 0xCD || t == 0xD1)     next = i + 3;
  else if (t == 0xCE || t == 0xD2 || t == 0xCA) next = i + 5;                                   // u32/i32/float32
  else if (t == 0xCF || t == 0xD3 || t == 0xCB) next = i + 9;                                   // u64/i64/float64
  else if (t == 0xDC || t == 0xDD || t == 0xDE || t == 0xDF) {
    // array16/32 and map16/32. A fields dict with sixteen or more entries is
    // no longer a fixmap, and refusing it aborted the whole parse — so a
    // message a real client considered ordinary was counted as not-an-LXMF-
    // message and went unproven. The header widths differ; the walk does not.
    const bool wide = (t == 0xDD || t == 0xDF);
    const bool map  = (t == 0xDE || t == 0xDF);
    const size_t hdr = wide ? 5 : 3;
    if (!need(hdr)) return false;
    size_t count = wide ? (((size_t)p[i+1] << 24) | ((size_t)p[i+2] << 16) |
                           ((size_t)p[i+3] << 8) | p[i+4])
                        : (((size_t)p[i+1] << 8) | p[i+2]);
    size_t members = count * (map ? 2u : 1u);
    if (members > n) return false;                    // more members than there are bytes
    next = i + hdr;
    if (depth == 0) return false;
    for (size_t k = 0; k < members; k++) {
      const uint8_t* iv = nullptr; size_t ivl = 0, inext = 0;
      if (!msgpackNext(p, n, next, iv, ivl, inext, depth - 1)) return false;
      next = inext;
    }
  }
  else if ((t & 0xF0) == 0x80 || (t & 0xF0) == 0x90) {
    // A container is not one element wide. Stepping over just its header left
    // a fields map's contents outside the payload, which is the same class of
    // fault as running past the end and just as fatal to a signature: the
    // bytes hashed were not the bytes signed. Its members are walked, by
    // length, without being interpreted — nothing here needs to know what a
    // fields map means, only where it stops.
    const bool map = (t & 0xF0) == 0x80;
    size_t members = (size_t)(t & 0x0F) * (map ? 2u : 1u);
    next = i + 1;
    if (depth == 0) return false;              // nesting deeper than LXMF uses; refuse rather than recurse
    for (size_t k = 0; k < members; k++) {
      const uint8_t* iv = nullptr; size_t ivl = 0, inext = 0;
      if (!msgpackNext(p, n, next, iv, ivl, inext, depth - 1)) return false;
      next = inext;
    }
  }
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
  const size_t elements = p[0] & 0x0F;
  if (elements < 3) return false;                        // the three we read
  size_t i = 1, next = 0; const uint8_t* v = nullptr; size_t vl = 0;
  // The timestamp. Read rather than skipped, because it is the only clock in
  // a message: a node without an RTC knows how long ago it took something in
  // but not when, and after a restart it does not know even that. It is the
  // sender's word for when they wrote it, which is what a reader wants and
  // not something to trust for anything else.
  if (i < n && p[i] == 0xCB && i + 9 <= n) {
    uint64_t bits = 0;
    for (int b = 0; b < 8; b++) bits = (bits << 8) | p[i + 1 + b];   // msgpack is big-endian
    memcpy(&out.sentAt, &bits, sizeof(out.sentAt));
  }
  if (!msgpackNext(p, n, i, v, vl, next)) return false;
  i = next;
  if (!msgpackNext(p, n, i, v, vl, next)) return false;  // title
  out.title = v; out.titleLen = vl; i = next;
  if (!msgpackNext(p, n, i, v, vl, next)) return false;  // content
  out.content = v; out.contentLen = vl; i = next;

  // The payload ends where its msgpack array ends, not where the packet does.
  // Taking "everything after the signature" was wrong: a packet can carry
  // trailing bytes the sender never signed, and hashing them makes a genuine
  // message look forged. It fails only for messages long enough to be padded,
  // which is why a short test verified and a real client's did not — the worst
  // kind of bug, correct on the case you try first.
  //
  // A msgpack array is self-delimiting, so the true end is found by walking
  // it. The remaining elements are skipped by length rather than read: this
  // file has no business interpreting a fields map, only knowing where it
  // stops.
  for (size_t k = 3; k < elements; k++) {
    if (!msgpackNext(p, n, i, v, vl, next)) return false;
    i = next;
  }
  out.payloadLen = i;
  return true;
}

// ---------------------------------------------------------------------------
// Writing a message, which is the same format read backwards.
//
// A node that can be administered over LXMF has to answer, and an answer is a
// message. Both directions live here for the reason the file exists at all:
// the reading side spent three bugs learning exactly which bytes LXMF signs,
// and a writing side with its own opinion about that would have to learn them
// again.
// ---------------------------------------------------------------------------

// The payload: [timestamp, title, content, fields]. The fields map is emitted
// empty — this node has nothing to put in one, and a reader that wants it can
// still walk past it.
inline size_t lxmfPayload(double sentAt, const char* title, const char* content,
                          uint8_t* out, size_t cap) {
  const size_t titleLen = title ? strlen(title) : 0;
  const size_t contentLen = content ? strlen(content) : 0;
  if (titleLen > 255 || contentLen > 255) return 0;      // bin8 is what this writes
  const size_t need = 1 + 9 + (2 + titleLen) + (2 + contentLen) + 1;
  if (need > cap) return 0;
  size_t i = 0;
  out[i++] = 0x94;                                       // fixarray of four
  out[i++] = 0xCB;                                       // float64, big-endian, as msgpack has it
  uint64_t bits = 0;
  memcpy(&bits, &sentAt, 8);
  for (int b = 7; b >= 0; b--) out[i++] = (uint8_t)(bits >> (8 * b));
  out[i++] = 0xC4; out[i++] = (uint8_t)titleLen;
  memcpy(out + i, title, titleLen); i += titleLen;
  out[i++] = 0xC4; out[i++] = (uint8_t)contentLen;
  memcpy(out + i, content, contentLen); i += contentLen;
  out[i++] = 0x80;                                       // an empty fixmap of fields
  return i;
}

// The bytes a signature covers, short of the hash LXMF appends to them. The
// caller adds SHA256 of this — the hash is the crypto library's, the layout is
// this file's, and keeping them apart is what lets the layout be tested on a
// host with no crypto at all.
//
// This is the one definition. Verifying a message that arrived builds the same
// bytes the same way; when it did not, every message from a sender this node
// knew was refused as a forgery, which is a worse failure than not checking.
inline size_t lxmfHashedPart(const uint8_t dest[16], const uint8_t source[16],
                             const uint8_t* payload, size_t payloadLen,
                             uint8_t* out, size_t cap) {
  const size_t need = 16 + 16 + payloadLen;
  if (need > cap) return 0;
  memcpy(out, dest, 16);
  memcpy(out + 16, source, 16);
  memcpy(out + 32, payload, payloadLen);
  return need;
}

// The envelope as it goes on the wire for a direct delivery: the two hashes,
// the signature, then the payload. An opportunistic send is this with the
// destination hash left off, because the destination it arrives at says what
// it was.
inline size_t lxmfEnvelope(const uint8_t dest[16], const uint8_t source[16],
                           const uint8_t sig[64], const uint8_t* payload, size_t payloadLen,
                           uint8_t* out, size_t cap, bool includeDest = true) {
  const size_t need = (includeDest ? 16u : 0u) + 16 + 64 + payloadLen;
  if (need > cap) return 0;
  size_t i = 0;
  if (includeDest) { memcpy(out + i, dest, 16); i += 16; }
  memcpy(out + i, source, 16); i += 16;
  memcpy(out + i, sig, 64);    i += 64;
  memcpy(out + i, payload, payloadLen);
  return need;
}

} // namespace Rns
