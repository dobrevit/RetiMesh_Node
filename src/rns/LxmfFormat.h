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
  size_t         fieldsCount;                   // entries in it, which is the question a caller actually has

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

// How many members a map or array claims, and where the first one starts.
// Counted in 64 bits and bounded against the buffer before use, for the reason
// msgpackNext learned: on a 32-bit target a claimed count near 2^31 wraps when
// doubled, and a header that should have been refused is skipped as empty.
inline bool msgpackContainerHeader(const uint8_t* p, size_t n, size_t i, bool wantMap,
                                   size_t& count, size_t& first) {
  if (i >= n) return false;
  const uint8_t t = p[i];
  const uint8_t fixMask = wantMap ? 0x80 : 0x90;
  const uint8_t wide16  = wantMap ? 0xDE : 0xDC;
  const uint8_t wide32  = wantMap ? 0xDF : 0xDD;
  if ((t & 0xF0) == fixMask) { count = t & 0x0F; first = i + 1; return true; }
  if (t == wide16) {
    if (i + 3 > n) return false;
    count = ((size_t)p[i + 1] << 8) | p[i + 2]; first = i + 3;
  } else if (t == wide32) {
    if (i + 5 > n) return false;
    const uint64_t c = ((uint64_t)p[i + 1] << 24) | ((uint64_t)p[i + 2] << 16) |
                       ((uint64_t)p[i + 3] << 8) | p[i + 4];
    if (c > n) return false;
    count = (size_t)c; first = i + 5;
  } else return false;
  return count <= n;
}

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
  else if (t == 0xDC || t == 0xDD || t == 0xDE || t == 0xDF ||
           (t & 0xF0) == 0x80 || (t & 0xF0) == 0x90) {
    // Every container, of either kind and any width. A fields dict with
    // sixteen or more entries is no longer a fixmap, and refusing it aborted
    // the whole parse — so a message a real client considered ordinary was
    // counted as not-an-LXMF-message and went unproven. The header widths
    // differ; the walk does not.
    //
    // A container is also not one element wide. Stepping over just its header
    // left a fields map's contents outside the payload, which is the same
    // class of fault as running past the end and just as fatal to a
    // signature: the bytes hashed were not the bytes signed. Its members are
    // walked, by length, without being interpreted — nothing here needs to
    // know what a fields map means, only where it stops.
    //
    // What a header says is decoded in one place, so this and the field walk
    // below cannot come to different conclusions about a container's shape
    // and silently disagree about where it ends.
    const bool map = (t == 0xDE || t == 0xDF || (t & 0xF0) == 0x80);
    size_t count = 0, first = 0;
    if (!msgpackContainerHeader(p, n, i, map, count, first)) return false;
    const size_t members = count * (map ? 2u : 1u);
    if (members > n) return false;                    // more members than there are bytes
    next = first;
    if (members && depth == 0) return false;          // deeper than LXMF nests; refuse rather than recurse
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
// The name goes out as msgpack *bin*, not str, and that is not a matter of
// taste. LXMF builds its own announce from `display_name.encode("utf-8")`,
// which packs as bin, and reads one back with `peer_data[0].decode("utf-8")`.
// A str unpacks in Python as `str`, which has no .decode — so the read throws,
// LXMF catches it, logs "Could not decode display name in included announce
// data" and returns None. Emitting a fixstr therefore made this node nameless
// in every client while putting an error line in every peer's log, on every
// announce. Our own reader takes either, which is exactly why a round-trip
// test could not see it.
inline size_t lxmfAppData(const char* name, int stampCost, uint8_t* out, size_t cap) {
  const size_t n = name ? strlen(name) : 0;
  // bin8 up to 255 bytes, which is far past any node name.
  // LXMF reads a cost as meaningful between 1 and 254. A cost outside that is
  // not a reason to fall silent: dropping the whole announce would take the
  // node's name and address off the network over one out-of-range field, so
  // it degrades to nil — which is what LXMF itself announces for a cost it
  // will not state.
  const bool wideCost = stampCost > 127 && stampCost < 255;
  const size_t need = 1                                  // array of 3
                    + 2 + n                               // bin8 header, name
                    + (wideCost ? 2u : 1u)                // cost, or nil
                    + 1;                                  // functionality list
  if (n > 255 || need > cap) return 0;
  uint8_t* p = out;
  *p++ = 0x93;                                           // fixarray of 3
  *p++ = 0xC4; *p++ = (uint8_t)n;                        // bin8
  if (n) memcpy(p, name, n);
  p += n;
  if (stampCost <= 0 || stampCost > 254) *p++ = 0xC0;    // nil — no stamp required
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

// How wide the character starting here is, or 0 if these bytes are not one.
//
// Names in an announce are UTF-8, and the ASCII-only guard this replaces
// rejected every byte above 0x7E — which is every byte of every accented,
// Cyrillic, Greek, CJK or emoji name there is, because those live entirely in
// the range it refused. A peer called "Café" or "Мартин" announced itself and
// appeared with no name at all.
//
// Rejecting is still the job, just of the right things: bytes that are not
// valid UTF-8, overlong forms, surrogate halves, and control characters —
// including the C1 range, which is where a name would hide an escape sequence
// that means something to a terminal reading the console.
// Two questions, and they are not the same one.
//
// utf8SeqLen asks only whether these bytes are a character — is this
// well-formed UTF-8, not overlong, not a surrogate half. It says nothing
// about whether anyone should look at it, so a newline is a character here.
// This is the question truncation asks: cutting a message body at a fixed
// byte count lands mid-sequence for most non-Latin text, and the answer has
// to be "where does the previous character end", not "is this printable" —
// holding a body to the second question would truncate every message at its
// first line break.
//
// utf8CharWidth asks the second question, and is what a name and a console
// line are held to.
inline size_t utf8SeqLen(const uint8_t* p, size_t n) {
  if (n == 0) return 0;
  const uint8_t c = p[0];
  auto cont = [&](size_t k) { return k < n && (p[k] & 0xC0) == 0x80; };
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0;                                // continuation byte, or an overlong two-byte form
  if (c < 0xE0) return cont(1) ? 2 : 0;
  if (c < 0xF0) {                                        // three
    if (!cont(1) || !cont(2)) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;              // overlong
    if (c == 0xED && p[1] >= 0xA0) return 0;             // UTF-16 surrogate half
    return 3;
  }
  if (c < 0xF5) {                                        // four
    if (!cont(1) || !cont(2) || !cont(3)) return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0;              // overlong
    if (c == 0xF4 && p[1] >= 0x90) return 0;             // past U+10FFFF
    return 4;
  }
  return 0;
}

inline size_t utf8CharWidth(const uint8_t* p, size_t n) {
  const size_t w = utf8SeqLen(p, n);
  if (w == 0) return 0;
  const uint8_t c = p[0];
  if (w == 1) return (c < 0x20 || c == 0x7F) ? 0 : 1;    // C0 controls and DEL
  if (w == 2) return (c == 0xC2 && p[1] < 0xA0) ? 0 : 2; // C1 controls
  if (w == 4) return 4;
  // Unicode's own control characters. Widening the guard from ASCII to UTF-8
  // let these back in, and they are the same attack the C0/C1 checks are for
  // rather than a different one: U+202E reverses the rest of the line it
  // lands on, so a name can make a console line or the neighbour list read as
  // something else entirely, and the zero-width and isolate characters hide
  // or reorder text while showing nothing at all.
  const uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                      ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
  if (cp >= 0x200B && cp <= 0x200F) return 0;            // zero-width and LTR/RTL marks
  if (cp >= 0x2028 && cp <= 0x202E) return 0;            // line/paragraph separators, bidi embedding and overrides
  if (cp >= 0x2060 && cp <= 0x2064) return 0;            // word joiner and invisible operators
  if (cp >= 0x2066 && cp <= 0x2069) return 0;            // bidi isolates
  if (cp == 0xFEFF) return 0;                            // zero-width no-break space
  return 3;
}

// Copy a name if it is one. All of it has to be displayable text — a name
// that is half valid is not a name, it is something else being read as one —
// but only as much as fits is copied, and never a character cut in half: a
// truncated sequence is not UTF-8, and it goes on to a display, a JSON
// document and a console that each have their own opinion about that.
inline size_t displayableName(const uint8_t* p, size_t n, char* out, size_t cap) {
  if (cap == 0) return 0;
  out[0] = '\0';
  if (!p || n == 0 || cap < 2) return 0;
  size_t fits = 0;
  for (size_t i = 0; i < n; ) {
    const size_t w = utf8CharWidth(p + i, n - i);
    if (w == 0) return 0;                                // not text; the caller shows nothing
    i += w;
    if (i <= cap - 1) fits = i;                          // last whole character that still fits
  }
  memcpy(out, p, fits); out[fits] = '\0';
  return fits;
}

// The same name, copied — validated and cut on a character boundary.
inline size_t lxmfName(const uint8_t* app, size_t len, char* out, size_t cap) {
  const uint8_t* v = nullptr; size_t vl = 0;
  if (!lxmfNameRef(app, len, v, vl)) { if (cap) out[0] = '\0'; return 0; }
  return displayableName(v, vl, out, cap);
}

// Message text is not a name and cannot be held to a name's standard: a body
// legitimately contains newlines, and refusing the whole message because one
// character is odd would throw away what the sender wrote. What it must not
// do is end mid-character. A body cut at a fixed byte count lands inside a
// multi-byte sequence about three times in four for non-Latin text, and the
// result is not UTF-8 — which reaches a JSON document and a display that each
// do something worse with it than showing a shorter message.
//
// Returns the longest length not exceeding max that ends on a character
// boundary. Bytes that are not valid UTF-8 at all stop the scan, so what
// comes back is always a well-formed prefix.
inline size_t utf8TrimLen(const uint8_t* p, size_t n, size_t max) {
  if (!p) return 0;
  size_t fits = 0;
  for (size_t i = 0; i < n; ) {
    const size_t w = utf8SeqLen(p + i, n - i);           // a boundary, not a judgement
    if (w == 0) break;
    i += w;
    if (i <= max) fits = i; else break;
  }
  return fits;
}

// The same text, copied somewhere it will be read by a person rather than
// stored — a log line on a serial console. Anything that is not a printable
// character becomes a dot, because the console is a terminal and a message
// body arrives from whoever is in earshot: an escape sequence in it would
// clear the operator's screen or hide the lines around it. Newlines go too,
// since this is one line.
inline size_t utf8SafeCopy(const uint8_t* p, size_t n, char* out, size_t cap) {
  if (cap == 0) return 0;
  out[0] = '\0';
  if (!p || n == 0 || cap < 2) return 0;
  size_t w = 0;
  for (size_t i = 0; i < n && w < cap - 1; ) {
    const size_t cw = utf8CharWidth(p + i, n - i);
    if (cw == 0) { out[w++] = '.'; i++; continue; }      // not a character; show that something was there
    if (w + cw > cap - 1) break;                          // never half a character
    memcpy(out + w, p + i, cw); w += cw; i += cw;
  }
  out[w] = '\0';
  return w;
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
    // How many entries, not how many bytes. A caller asking "did this message
    // bring anything with it" is asking about entries, and the two answers
    // differ: an empty dict encoded as map16 is three bytes rather than one,
    // so a length test calls a plain text message an attachment.
    const uint8_t h = p[at];
    if ((h & 0xF0) == 0x80)   out.fieldsCount = h & 0x0F;                    // fixmap
    else if (h == 0xDE && at + 3 <= n)
      out.fieldsCount = ((size_t)p[at + 1] << 8) | p[at + 2];                // map16
    else if (h == 0xDF && at + 5 <= n)
      out.fieldsCount = ((size_t)p[at + 1] << 24) | ((size_t)p[at + 2] << 16) |
                        ((size_t)p[at + 3] << 8) | p[at + 4];                // map32
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

// The bytes the sender signed, in order, handed to a sink one span at a time.
//
// This is the whole of interoperability in four lines, so there is one copy of
// it and both the node and its tests ask this rather than each remembering the
// order. A test that rebuilt the same sequence for itself could not fail when
// the node's copy changed — which is the one thing that test exists to catch.
//
// Spans rather than a buffer because the caller owns the storage: a message
// arriving as a resource is kilobytes, which does not belong on this stack.
template <typename Sink>
inline void lxmfSignedSpans(const LxmfMessage& m, Sink&& sink) {
  sink(m.destHash, (size_t)16);
  sink(m.sourceHash, (size_t)16);
  sink(&m.signedHeader, (size_t)1);
  sink(m.signedBody, m.signedBodyLen);
}

// ---------------------------------------------------------------------------
//  Reading inside the fields map
//
//  parseLxmf reports where the fields map is and how many entries it holds and
//  deliberately reads none of them: most of what LXMF carries there is none of
//  a transport node's business, and a parser with an opinion about every field
//  would be a parser with far more surface than a node needs.
//
//  What is worth reading is the commands a client sends — a ping, an echo, a
//  request for the signal this node heard them at. Those are what a person
//  standing in a field with a phone actually wants to ask a node, and
//  answering costs one short message.
// ---------------------------------------------------------------------------

// A msgpack unsigned integer, which is what a field key and a command id are.
// False for anything else, negative fixints included: a field id is never
// negative, and reading one as though it were is how a map walk loses its
// place and starts reading values as keys.
inline bool msgpackUint(const uint8_t* p, size_t n, size_t i, uint32_t& out, size_t& next) {
  if (i >= n) return false;
  const uint8_t t = p[i];
  if (t <= 0x7F) { out = t; next = i + 1; return true; }                       // positive fixint
  if (t == 0xCC) { if (i + 2 > n) return false; out = p[i + 1]; next = i + 2; return true; }
  if (t == 0xCD) { if (i + 3 > n) return false;
                   out = ((uint32_t)p[i + 1] << 8) | p[i + 2]; next = i + 3; return true; }
  if (t == 0xCE) { if (i + 5 > n) return false;
                   out = ((uint32_t)p[i + 1] << 24) | ((uint32_t)p[i + 2] << 16) |
                         ((uint32_t)p[i + 3] << 8) | p[i + 4]; next = i + 5; return true; }
  return false;
}

// One value out of a fields map, by its LXMF field id. What comes back is the
// raw extent of the value — its type byte and everything it occupies — because
// the caller may want to walk into it (a commands array) or read it as text,
// and this file should not have to know which in advance.
inline bool lxmfField(const uint8_t* fields, size_t n, uint32_t id,
                      const uint8_t*& val, size_t& valLen) {
  val = nullptr; valLen = 0;
  size_t count = 0, i = 0;
  if (!fields || !msgpackContainerHeader(fields, n, 0, /*wantMap*/ true, count, i)) return false;
  for (size_t k = 0; k < count; k++) {
    uint32_t key = 0; size_t afterKey = 0;
    // A key this node did not write may be of any type. Skipping it by length
    // keeps the walk in step; abandoning the map would lose every field after
    // the first odd one.
    const bool intKey = msgpackUint(fields, n, i, key, afterKey);
    if (!intKey) {
      const uint8_t* kv = nullptr; size_t kvl = 0;
      if (!msgpackNext(fields, n, i, kv, kvl, afterKey)) return false;
    }
    const uint8_t* vv = nullptr; size_t vvl = 0, afterVal = 0;
    if (!msgpackNext(fields, n, afterKey, vv, vvl, afterVal)) return false;
    if (intKey && key == id) { val = fields + afterKey; valLen = afterVal - afterKey; return true; }
    i = afterVal;
  }
  return false;
}

// The field that carries them, and the commands this node understands. The
// numbers are Sideband's, because they are what its buttons send.
static const uint32_t kFieldCommands    = 0x09;
static const uint32_t kCommandTelemetry = 0x01;
static const uint32_t kCommandPing      = 0x02;
static const uint32_t kCommandEcho      = 0x03;
static const uint32_t kCommandSignal    = 0x04;

struct LxmfCommand {
  uint32_t       id;                     // one of the kCommand* above
  const uint8_t* text;                   // the argument where it is text or bytes, else null
  size_t         textLen;
};

// The commands out of a FIELD_COMMANDS value: an array whose members are each
// a map of one entry, {command id: argument}. Anything not of that shape is
// skipped rather than refused — a client is free to send commands this node
// has never heard of, and the ones it does understand should still work.
inline size_t lxmfCommands(const uint8_t* val, size_t n, LxmfCommand* out, size_t max) {
  if (!val || !out || max == 0) return 0;
  size_t count = 0, i = 0, found = 0;
  if (!msgpackContainerHeader(val, n, 0, /*wantMap*/ false, count, i)) return 0;
  for (size_t k = 0; k < count && found < max; k++) {
    const uint8_t* ev = nullptr; size_t evl = 0, afterElem = 0;
    if (!msgpackNext(val, n, i, ev, evl, afterElem)) return found;   // bounds come from here
    size_t pairs = 0, at = 0;
    if (msgpackContainerHeader(val, n, i, /*wantMap*/ true, pairs, at) && pairs >= 1) {
      uint32_t id = 0; size_t afterKey = 0;
      if (msgpackUint(val, n, at, id, afterKey)) {
        const uint8_t* av = nullptr; size_t avl = 0, afterVal = 0;
        if (msgpackNext(val, n, afterKey, av, avl, afterVal)) {
          out[found].id = id;
          out[found].text = av;        // msgpackNext sets this only for a string or binary
          out[found].textLen = avl;
          found++;
        }
      }
    }
    i = afterElem;
  }
  return found;
}

// ---------------------------------------------------------------------------
// Writing a message, which is the same format read backwards.
//
// A node that can be administered over LXMF has to answer, and an answer is a
// message. Both directions live here for the reason the file exists at all:
// the reading side spent four bugs learning exactly which bytes LXMF signs,
// and a writing side with its own opinion about that would have to learn them
// again. It does not have one — it builds an LxmfMessage describing what it is
// about to send and hands that to lxmfSignedSpans, the same call the reading
// side uses, so the two cannot come to different conclusions about the order.
// ---------------------------------------------------------------------------

// The payload: [timestamp, title, content, fields]. The fields map is emitted
// empty and no stamp is appended, so what is signed is the whole of what is
// sent — the case the reading side has to work to reconstruct is one this
// side simply never creates.
// `fields` is a msgpack map, already encoded, or null for a message that
// carries only its text. It is taken pre-encoded rather than built here
// because what goes in it — a telemetry document, one day something else — is
// not this file's business, and because the caller has already had to decide
// how much room to give it.
inline size_t lxmfPayload(double sentAt, const char* title, const char* content,
                          uint8_t* out, size_t cap,
                          const uint8_t* fields = nullptr, size_t fieldsLen = 0) {
  const size_t titleLen = title ? strlen(title) : 0;
  const size_t contentLen = content ? strlen(content) : 0;
  if (titleLen > 255 || contentLen > 255) return 0;      // bin8 is what this writes
  const size_t fieldBytes = (fields && fieldsLen) ? fieldsLen : 1;
  const size_t need = 1 + 9 + (2 + titleLen) + (2 + contentLen) + fieldBytes;
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
  if (fields && fieldsLen) { memcpy(out + i, fields, fieldsLen); i += fieldsLen; }
  else                       out[i++] = 0x80;            // an empty fixmap of fields
  return i;
}

// An outgoing message described the way an incoming one is, so that the bytes
// to sign come from lxmfSignedSpans rather than from a second opinion here.
// Nothing this side emits is stamped, so the signed header is the array header
// as written and the signed body is everything after it.
inline LxmfMessage lxmfOutgoing(const uint8_t dest[16], const uint8_t source[16],
                                const uint8_t* payload, size_t payloadLen) {
  LxmfMessage m{};
  m.destHash = dest;
  m.sourceHash = source;
  m.payload = payload;
  m.payloadLen = payloadLen;
  m.stamped = false;
  m.signedHeader = payloadLen ? payload[0] : (uint8_t)0x94;
  m.signedBody = payloadLen ? payload + 1 : payload;
  m.signedBodyLen = payloadLen ? payloadLen - 1 : 0;
  return m;
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
