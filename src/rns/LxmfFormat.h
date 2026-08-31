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
//  key — from an announce this node heard, or from the sender identifying
//  itself on its link — which is not a question about bytes
//  (RnsTransport::handleLxmfMessage). What this file owes that decision is
//  the exact bytes the sender signed, which are not always the bytes that
//  arrived; see signedHeader below.
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
// hash*: hashed_part = dest || source || the payload the sender hashed, and
// the signed data is hashed_part || SHA256(hashed_part). Getting either half
// wrong does not fail safely — it refuses every message a real client sent,
// as a forgery, which accuses the honest sender. It binds the message to the
// pair of addresses either way: replayed at another node it does not verify,
// and neither does an altered text.
//
// "The payload the sender hashed" is doing real work in that sentence, and is
// not the same as the payload that arrived whenever a stamp is present. See
// signedHeader.
struct LxmfMessage {
  const uint8_t* destHash;               // 16 bytes
  const uint8_t* sourceHash;             // 16
  const uint8_t* signature;              // 64
  const uint8_t* payload;                // msgpack, what the signature covers with the two hashes
  size_t         payloadLen;
  const uint8_t* title;   size_t titleLen;      // may be empty; LXMF allows it
  const uint8_t* content; size_t contentLen;
  double         sentAt;                        // the sender's clock, seconds since the epoch; 0 if absent

  // The fields map, whole and uninterpreted. Everything LXMF carries beyond a
  // subject and a body lives here — attachments, telemetry, commands, the
  // ticket that lets a peer reply without doing stamp work — so a node that
  // never looks at it shows a photo as an empty message. Handing over its
  // extent lets a caller say "this one carried something" without this file
  // growing an opinion about what.
  const uint8_t* fields;  size_t fieldsLen;     // empty when the message had none

  // What the sender signed, which is not the same bytes as what arrived.
  //
  // LXMF hashes the payload as it stood before a stamp was appended: it
  // unpacks, drops everything past the fourth element, and re-packs the four.
  // A receiver that hashes the array as received disagrees with the sender
  // about the message, and the signature cannot match — which is not a
  // failure that looks like a bug, it looks like the honest sender forging.
  //
  // Re-encoding those four elements would mean trusting this file's msgpack
  // encoder to agree with the sender's byte for byte. It does not have to:
  // the elements are already canonical, having come from that encoder, so the
  // bytes are reused as received and only the array header is replaced.
  // Hashing them means header first, then body.
  uint8_t        signedHeader;                  // 0x94 when a stamp was excluded, else the array header as received
  const uint8_t* signedBody;                    // the elements it covers, verbatim
  size_t         signedBodyLen;
  bool           stamped;                       // a stamp was present, and is excluded above
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

// The app_data of an lxmf.delivery announce: three elements, and the only
// place this node states what it can do.
//
//     [ display name, stamp cost, [ supported functionality ] ]
//
// Two of those were being got wrong, and both faults land on the sender.
//
// The third element is not optional in practice. A peer reading a list
// shorter than three assumes every capability it knows of, and the one that
// matters is compression: it will then bz2 anything it sends that is large
// enough to travel as a resource, which microReticulum has no decompressor
// for and refuses outright. An empty list is how a node says "none of them".
// Put SF_COMPRESSION (0x00) in it on the day there is a decompressor, and not
// before — claiming it without one is how long messages disappear.
//
// A stampCost of zero or less means no stamp is required, and it is written
// as msgpack nil rather than as the number 0. That is not a cosmetic
// difference: LXMF skips stamp generation only when the announced cost is
// nil. A literal 0 is read as a cost, and one satisfied by the first value
// tried, so the sender attaches a stamp — which arrives as a fifth payload
// element that the sender itself excluded from what it signed. Announcing 0
// therefore made every message from a current client arrive unverifiable, and
// reported the honest sender as a signature mismatch.
inline size_t lxmfAppData(const char* name, int stampCost, uint8_t* out, size_t cap) {
  const size_t n = name ? strlen(name) : 0;
  // fixstr carries up to 31 bytes and is what every LXMF client emits for a
  // node name; str8 covers the rest. Nothing here needs more than 255.
  const bool fixstr = n <= 31;
  // LXMF reads a cost as meaningful between 1 and 254; above a fixint's 127
  // it needs a uint8 header.
  const bool wideCost = stampCost > 127;
  const size_t need = 1                                  // array of 3
                    + (fixstr ? 1 : 2) + n                // name
                    + (wideCost ? 2u : 1u)                // cost, or nil
                    + 1;                                  // functionality list
  if (n > 255 || stampCost > 254 || need > cap) return 0;
  uint8_t* p = out;
  *p++ = 0x93;                                           // fixarray of 3
  if (fixstr) *p++ = (uint8_t)(0xA0 | n);
  else      { *p++ = 0xD9; *p++ = (uint8_t)n; }
  memcpy(p, name, n); p += n;
  if (stampCost <= 0)  *p++ = 0xC0;                      // nil — no stamp required
  else if (!wideCost)  *p++ = (uint8_t)stampCost;        // positive fixint
  else               { *p++ = 0xCC; *p++ = (uint8_t)stampCost; }
  *p++ = 0x90;                                           // empty fixarray: no functionality claimed
  return (size_t)(p - out);
}

// Where the name sits inside an LXMF announce's app_data — the other
// direction of lxmfAppData(), and here beside it for the same reason. No
// copy, because a caller that has to judge the bytes (are they text? do they
// fit?) needs them where they are rather than in a buffer it has already
// committed to. False when the app_data is not an LXMF array at all, which
// leaves the caller to treat it as whatever else it might be.
inline bool lxmfNameRef(const uint8_t* app, size_t len,
                        const uint8_t*& name, size_t& nameLen) {
  if (!app || len < 2) return false;
  if ((app[0] & 0xF0) != 0x90) return false;             // not the array LXMF sends
  const uint8_t* v = nullptr; size_t vl = 0, next = 0;
  if (!msgpackNext(app, len, 1, v, vl, next) || !v || vl == 0) return false;
  name = v; nameLen = vl;
  return true;
}

// The same name, copied.
inline size_t lxmfName(const uint8_t* app, size_t len, char* out, size_t cap) {
  const uint8_t* v = nullptr; size_t vl = 0;
  if (cap < 2 || !lxmfNameRef(app, len, v, vl)) return 0;
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

  // The fields map. Walked by length rather than interpreted — this file has
  // no business knowing what a telemetry reading or an attachment is — but its
  // extent is kept, so a caller can tell a message that carried something from
  // one that did not.
  size_t signedEnd = i;
  if (elements >= 4) {
    const size_t at = i;
    if (!msgpackNext(p, n, i, v, vl, next)) return false;
    out.fields = p + at; out.fieldsLen = next - at;
    i = next;
    signedEnd = i;
  }

  // Anything past the fourth element is not part of what the sender signed.
  // LXMF appends a stamp there — proof of work against a cost the receiver
  // announced — after computing the hash, and drops it again before checking
  // one. A receiver that hashes it too disagrees with the sender about the
  // message and refuses it as a forgery.
  //
  // Skipping the stamp is what makes this correct rather than merely lucky:
  // announcing no cost (lxmfAppData) stops most senders attaching one, but a
  // sender holding a ticket for this node attaches a stamp whatever the cost
  // says, and that message has to verify too.
  for (size_t k = 4; k < elements; k++) {
    if (!msgpackNext(p, n, i, v, vl, next)) return false;
    i = next;
  }

  // The payload ends where its msgpack array ends, not where the packet does.
  // Taking "everything after the signature" was wrong: a packet can carry
  // trailing bytes the sender never signed, and hashing them makes a genuine
  // message look forged. It fails only for messages long enough to be padded,
  // which is why a short test verified and a real client's did not — the worst
  // kind of bug, correct on the case you try first.
  out.payloadLen = i;

  // The array as the sender hashed it: the first four elements under a header
  // that says four, whatever the header on the wire said.
  out.stamped      = elements > 4;
  out.signedHeader = out.stamped ? (uint8_t)0x94 : p[0];
  out.signedBody   = p + 1;
  out.signedBodyLen = signedEnd - 1;
  return true;
}

} // namespace Rns
