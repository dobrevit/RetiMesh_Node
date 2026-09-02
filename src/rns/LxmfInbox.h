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
//  LxmfInbox.h — the last fifty messages, on the filesystem rather than in RAM
//
//  What a node received is the one thing that says LXMF works end to end, and
//  before this it kept exactly one message, in RAM, overwritten by the next.
//  Fifty is enough to see a conversation; the questions are where they live
//  and what that costs.
//
//  Not in RAM. Fifty records is about ten kilobytes, and the Wireless Stick
//  runs with under six kilobytes of byte-addressable DRAM free — a resident
//  ring would take that board out on its own. So the log is on the store,
//  read back only when something asks, and what is held in memory is the next
//  sequence number and a short queue of messages not yet written.
//
//  Not gated on the SD card either, though that was the obvious instinct.
//  Flash wear does not justify it: these modules specify 100,000 erase cycles
//  per 4 KB sector, the LittleFS partition is 224 sectors on the 4 MB boards
//  and 1,248 on the 8 MB ones, and a two-hundred-byte record costs well under
//  one erase. Charging a whole sector erase to every message still buys twenty
//  million messages on the smallest board. What already writes to this flash
//  all day is the announce store — Identity::remember() puts a record and
//  microStore flushes it there and then, for every announce the node hears and
//  keeps, thousands a day on a live fleet.
//
//  That arithmetic holds for messages a person sends. It does not hold on its
//  own, and saying it did was the mistake worth naming here: the delivery
//  address is reachable over the node's own TCP interface and over
//  AutoInterface on the LAN, and neither authenticates. Somebody on the access
//  point can send as fast as the link allows, and at fifty a second the twenty
//  million is a week rather than a lifetime. So arrivals are rate limited
//  before they reach the store (kInboxBurst, kInboxRefillMs) and repeats of a
//  message already stored are not stored twice. Under anything a person does
//  neither ever engages; under a flood the ring keeps turning and the flash
//  does not pay for it.
//
//  The file is fifty fixed-size slots and a sequence number that only ever
//  goes up: slot = seq % 50, so writing the fifty-first message overwrites the
//  first, in place, with no rewriting of the file and no compaction. The
//  newest message is the highest sequence number, which also survives the
//  reboot that resets millis().
//
//  This header holds the format, the arithmetic and the rules both readers
//  share, so all of it is tested on the host. The file itself is in
//  LxmfInbox.cpp.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Rns {

// Fifty is what an operator can scroll; the cost is ten kilobytes of store.
static const size_t kInboxSlots = 50;

// Longer than a message anyone types at a node and shorter than the frame
// this has to fit in. A message beyond it is kept truncated rather than
// dropped: the point of the log is that something arrived and who from.
static const size_t kInboxTextMax = 160;

// Every record is this size so the slot arithmetic is a multiplication. It
// is also what makes a partly written record recognisable: a record whose
// declared text length does not fit is not read.
static const size_t kInboxRecordSize = 200;

// How many records one request may ask for, and how many it gets when it does
// not say. Not the ring depth, which is what it used to be: fifty records
// rendered as JSON is over ten kilobytes of document, then about as much again
// serialised into a String, on the heap of a board that may have five
// kilobytes to spare. A reader that wants all fifty pages back through them,
// which costs it nothing and costs the node one screenful at a time.
static const size_t kInboxPageMax     = 16;
static const size_t kInboxPageDefault = 12;

// Rate limiting for what reaches the store. A bucket of kInboxBurst, refilled
// one token every kInboxRefillMs: a person messaging a node never empties it,
// and a flood settles to twelve writes a minute — about seventeen thousand a
// day against a budget of twenty million, so even a sustained attack is years
// rather than a week.
static const uint32_t kInboxBurst    = 8;
static const uint32_t kInboxRefillMs = 5000;

// How many recent messages are remembered well enough to recognise a repeat.
// LXMF retransmits when the sender does not see a proof, which over LoRa is
// ordinary; without this one message from one person takes several of the
// fifty slots and pushes other people's out.
static const size_t kInboxRecentSigs = 8;

// What this node concluded about the sender. The three are kept apart
// because they mean different things and the difference is the whole point
// of showing them: only the first will ever be allowed to drive anything.
enum InboxStanding : uint8_t {
  StandingVerified = 0,   // heard the sender announce, and the signature matched
  StandingNoKey    = 1,   // never heard them announce, so there was nothing to check
  StandingMismatch = 2,   // heard them, and the signature did not match
};

// How it got here. An operator chasing a delivery that did not arrive wants
// to know which of the three paths carried the ones that did.
enum InboxVia : uint8_t {
  ViaPacket   = 0,        // one packet straight to the destination
  ViaLink     = 1,        // over a link the client established
  ViaResource = 2,        // over a link, packed as a resource
};

struct InboxRecord {
  uint32_t seq;                    // 0 marks a slot never written; otherwise counts up for ever
  uint32_t bootId;                 // which run of the node took it in
  uint32_t bootMs;                 // millis() then, so "ago" is honest within one run
  double   sentAt;                 // the sender's own clock, from the message payload
  uint8_t  from[16];               // source hash
  uint8_t  standing;               // InboxStanding
  uint8_t  via;                    // InboxVia
  uint16_t textLen;                // bytes of text actually stored
  char     text[kInboxTextMax];    // not terminated; textLen is the length
};

// Which slot a sequence number is written to. Sequence numbers start at 1 so
// that zero can mean "never written" in a slot that has not been reached.
inline size_t inboxSlot(uint32_t seq) { return (size_t)((seq - 1) % kInboxSlots); }

// A timestamp off the wire is a stranger's float, and nothing upstream checks
// it: parseLxmf copies the eight bytes the sender sent. Converting a NaN, an
// infinity or 1e300 to an integer is undefined behaviour, and the integer is
// what the console prints to an operator — so anything that is not a plausible
// epoch second becomes zero, which the readers already show as "no clock".
// The comparison is written so that NaN fails it: NaN > 0.0 is false.
inline double inboxSentAt(double t) {
  if (!(t > 0.0) || t > 4.0e9) return 0.0;
  return t;
}

// The record laid out by hand rather than copied out of the struct: the file
// outlives a compiler's opinion about padding, and a store can move between
// LittleFS and an SD card without being rewritten.
//
//   0   seq        u32 le
//   4   bootId     u32 le
//   8   bootMs     u32 le
//   12  sentAt     f64 le
//   20  from       16 bytes
//   36  standing   u8
//   37  via        u8
//   38  textLen    u16 le
//   40  text       160 bytes
inline void encodeInbox(const InboxRecord& r, uint8_t* out) {
  memset(out, 0, kInboxRecordSize);
  auto u32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
  };
  u32(out + 0, r.seq);
  u32(out + 4, r.bootId);
  u32(out + 8, r.bootMs);
  const double t = inboxSentAt(r.sentAt);
  uint64_t bits = 0;
  memcpy(&bits, &t, 8);
  for (int i = 0; i < 8; i++) out[12 + i] = (uint8_t)(bits >> (8 * i));
  memcpy(out + 20, r.from, 16);
  out[36] = r.standing;
  out[37] = r.via;
  const uint16_t n = r.textLen > kInboxTextMax ? (uint16_t)kInboxTextMax : r.textLen;
  out[38] = (uint8_t)n; out[39] = (uint8_t)(n >> 8);
  memcpy(out + 40, r.text, n);
}

// False for a slot never written and for one that does not make sense —
// a length past the end of the record, or a standing this build does not
// know. A store is a file on a card someone can move between nodes, so what
// comes back off it is checked rather than trusted.
inline bool decodeInbox(const uint8_t* in, InboxRecord& r) {
  auto u32 = [](const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  };
  r = InboxRecord{};
  r.seq = u32(in + 0);
  if (r.seq == 0) return false;
  r.bootId = u32(in + 4);
  r.bootMs = u32(in + 8);
  uint64_t bits = 0;
  for (int i = 0; i < 8; i++) bits |= (uint64_t)in[12 + i] << (8 * i);
  memcpy(&r.sentAt, &bits, 8);
  r.sentAt = inboxSentAt(r.sentAt);            // a file is as foreign as a packet
  memcpy(r.from, in + 20, 16);
  r.standing = in[36];
  r.via = in[37];
  if (r.standing > StandingMismatch || r.via > ViaResource) return false;
  r.textLen = (uint16_t)((uint16_t)in[38] | ((uint16_t)in[39] << 8));
  if (r.textLen > kInboxTextMax) return false;
  memcpy(r.text, in + 40, r.textLen);
  return true;
}

// How many records a request asked for. One rule, because there were two: the
// console refused "MESSAGES 99" while the page quietly returned twelve rows
// for "?n=99", which is two answers to one question from one node, and the
// next change to the cap would have had to be made twice.
//
// Empty or absent means the default. Anything else must be a whole number in
// range; false is a refusal, and both callers say so rather than guessing.
inline bool inboxPageSize(const char* raw, size_t& out) {
  if (!raw || !*raw) { out = kInboxPageDefault; return true; }
  size_t v = 0;
  for (const char* p = raw; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    v = v * 10 + (size_t)(*p - '0');
    if (v > kInboxPageMax) return false;       // also stops a long digit string overflowing
  }
  if (v < 1) return false;
  out = v;
  return true;
}

// The words the console and the web page both use, so the two cannot drift
// into describing the same message differently.
inline const char* standingName(uint8_t s) {
  return s == StandingVerified ? "verified"
       : s == StandingNoKey    ? "unverified"
       : s == StandingMismatch ? "mismatch"
                               : "unknown";
}

inline const char* viaName(uint8_t v) {
  return v == ViaPacket   ? "packet"
       : v == ViaLink     ? "link"
       : v == ViaResource ? "resource"
                          : "unknown";
}

// ---------------------------------------------------------------------------
// The file. Everything above is arithmetic; everything here touches the store.
//
// Arrivals and reads run on different tasks, so they are kept apart rather
// than serialised against each other: a message arriving on the Reticulum task
// is queued, and the loop task writes it. Flash is slow — a block erase is
// tens of milliseconds and can be a hundred — and that time used to sit
// between decoding a message and proving it, delaying the sender's receipt and
// stalling the Reticulum job loop that link and receipt timeouts run on.
// ---------------------------------------------------------------------------
namespace Inbox {

// Finds where the log left off. Safe to call when there is no file yet.
void begin();

// Takes one message, for writing later. Returns false when it was refused —
// a repeat of something already stored, or more than the rate limit allows —
// which the caller counts rather than retries.
//
// `sigPrefix` is the first eight bytes of the message signature, which is what
// makes a retransmission recognisable as one.
bool note(const uint8_t from[16], uint8_t standing, uint8_t via, double sentAt,
          const char* text, size_t textLen, uint64_t sigPrefix);

// Writes what note() queued. Called from the loop task; does nothing when
// there is nothing waiting.
void poll();

// How many are stored, the newest sequence number (0 when empty), and how many
// arrivals were refused since boot.
uint32_t stored();
uint32_t newest();
uint32_t dropped();

// One record, for the caller that wants only the newest.
bool read(uint32_t seq, InboxRecord& out);

// The newest arrival since last asked, exactly once — the display's
// full-screen interrupt eats this; everything else keeps reading pages.
bool takeNotice(InboxRecord& out);

struct Page {
  size_t   count  = 0;      // how many records fn() was actually given
  uint32_t oldest = 0;      // the sequence number of the last one
  bool     more   = false;  // whether anything older than that is still stored
};

// Up to `max` records, newest first, starting at `fromSeq` (0 means "the
// newest there is"). One open and one lock for the whole page rather than one
// of each per record: the page handler runs on the network task, and thirteen
// separate takes behind a writer mid-erase is thirteen chances to stall every
// other HTTP client.
Page readPage(uint32_t fromSeq, size_t max, void (*fn)(const InboxRecord&, void*), void* ctx);

// This run of the node, for telling "four minutes ago" from "some time before
// the last restart".
uint32_t bootId();

} // namespace Inbox
} // namespace Rns
