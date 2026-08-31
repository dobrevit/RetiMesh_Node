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
//  LxmfInbox.cpp — the fifty slots, on whichever filesystem the store is on
//
//  The format and the arithmetic are in the header and tested on the host.
//  What is here is the file: where it lives, the queue that keeps flash off
//  the Reticulum task, and the lock that lets a write and a page being served
//  happen at once without either seeing half a record.
// ============================================================================
#include "LxmfInbox.h"
#include "RnsFileSystem.h"
#include "Lock.h"
#include <esp_random.h>
#include <Arduino.h>
#include <freertos/queue.h>

namespace Rns {
namespace Inbox {

// Beside the transport's own store, under the same root, so it follows the
// store between LittleFS and the card without a second decision about where
// storage lives.
static const char* kPath = RNS_FS_ROOT "/inbox.log";
static const size_t kFileSize = kInboxSlots * kInboxRecordSize;

static SemaphoreHandle_t sLock = nullptr;
static QueueHandle_t     sQueue = nullptr;    // arrivals waiting to be written
static uint32_t sNewest = 0;         // highest sequence number stored; 0 when empty
static uint32_t sStored = 0;         // how many slots hold a message
static uint32_t sDropped = 0;        // arrivals refused: repeats and floods
static uint32_t sBootId = 0;
static bool     sReady  = false;

// The rate limiter's bucket, and the signatures of the last few messages
// stored. Both are touched only from the task that calls note(), which is the
// one that takes messages in.
static uint32_t sTokens   = kInboxBurst;
static uint32_t sLastFill = 0;
static uint64_t sRecent[kInboxRecentSigs] = {0};
static size_t   sRecentAt = 0;

uint32_t bootId()  { return sBootId; }
uint32_t stored()  { return sStored; }
uint32_t newest()  { return sNewest; }
uint32_t dropped() { return sDropped; }

// The file is laid out full-size on first use so that every later write is an
// overwrite of one record. Growing it a record at a time would work, but it
// would mean the wrap is the first time the file reaches its final size — and
// the wrap is the case least likely to be exercised before a release.
//
// A file that is already there is only accepted at its full length. A layout
// that ran out of room leaves a short file behind, and taking that as "already
// done" on every later boot would run the node on a ring with slots that are
// not there — believed empty when read, and silently lost when written.
// How big the file is, asked with it closed. A File still open for writing
// answers 0 — the length is what the directory entry says, and that is not
// updated until the close — so asking mid-write says the layout failed when it
// succeeded, and the inbox then stays down for the whole of that boot with
// messages arriving and going nowhere.
static size_t fileSize(fs::FS& fs) {
  File f = fs.open(kPath, FILE_READ);
  const size_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

// Writes the fifty blank slots. Full-size on first use so that every later
// write is an overwrite of one record: growing it a record at a time would
// work, but it would mean the wrap is the first time the file reaches its
// final size — and the wrap is the case least likely to be exercised before a
// release.
static bool layout(fs::FS& fs) {
  File f = fs.open(kPath, FILE_WRITE);
  if (!f) {
    log_e("inbox: cannot create %s; messages will not be kept", kPath);
    return false;
  }
  uint8_t blank[kInboxRecordSize];
  memset(blank, 0, sizeof(blank));
  bool ok = true;
  for (size_t i = 0; i < kInboxSlots && ok; i++)
    ok = f.write(blank, sizeof(blank)) == sizeof(blank);
  f.close();
  const size_t wrote = ok ? fileSize(fs) : 0;
  if (wrote != kFileSize) {
    log_e("inbox: could not lay out %s (%u of %u bytes); the store may be full",
          kPath, (unsigned)wrote, (unsigned)kFileSize);
    return false;
  }
  return true;
}

// A file that is already there is only accepted at its full length. A layout
// that ran out of room leaves a short file behind, and taking that as "already
// done" on every later boot would run the node on a ring with slots that are
// not there — believed empty when read, and silently lost when written.
static bool ensureFile(fs::FS& fs) {
  if (fs.exists(kPath)) {
    const size_t have = fileSize(fs);
    if (have == kFileSize) return true;
    log_w("inbox: %s is %u bytes, not %u; laying it out again",
          kPath, (unsigned)have, (unsigned)kFileSize);
  }
  return layout(fs);
}

void begin() {
  if (!sLock) sLock = xSemaphoreCreateMutex();
  if (!sQueue) sQueue = xQueueCreate(3, sizeof(InboxRecord));
  // Distinguishes this run from the last one. A message's "four minutes ago"
  // is only true within the run that took it in, because millis() starts
  // again at every restart; anything older than the current run is shown by
  // the sender's own clock instead.
  sBootId = esp_random();
  if (!sBootId) sBootId = 1;                  // 0 would read as "no boot recorded"
  sLastFill = millis();

  Sys::Lock held(sLock);
  fs::FS& fs = *RnsFileSystem::backend();
  if (!ensureFile(fs)) return;

  // Where the log left off. Only the sequence number of each slot is read —
  // fifty four-byte reads rather than the whole ten kilobytes — because the
  // rest of a record is not needed to know which one is newest.
  File f = fs.open(kPath, FILE_READ);
  if (!f) return;
  size_t unreadable = 0;
  for (size_t slot = 0; slot < kInboxSlots; slot++) {
    uint8_t b[4];
    // One slot that will not read is one slot lost, not a reason to abandon
    // the other forty-nine. Stopping here used to leave sNewest at zero with
    // the store still marked usable, which reads as an empty inbox and then
    // hands the next message a sequence number that is already in use —
    // destroying a record that was perfectly intact.
    if (!f.seek(slot * kInboxRecordSize) || f.read(b, 4) != 4) { unreadable++; continue; }
    const uint32_t seq = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (!seq) continue;
    sStored++;
    if (seq > sNewest) sNewest = seq;
  }
  f.close();
  if (unreadable) log_w("inbox: %u of %u slots could not be read", (unsigned)unreadable, (unsigned)kInboxSlots);
  sReady = true;
  log_i("inbox: %lu message(s) kept on %s, newest #%lu",
        (unsigned long)sStored, RnsFileSystem::backendName(), (unsigned long)sNewest);
}

// A token for this arrival, or nothing. The bucket refills on the clock rather
// than on a timer, so an idle node is always full when the next message comes.
static bool takeToken() {
  const uint32_t now = millis();
  const uint32_t due = (now - sLastFill) / kInboxRefillMs;
  if (due) {
    sTokens = sTokens + due > kInboxBurst ? kInboxBurst : sTokens + due;
    sLastFill += due * kInboxRefillMs;
  }
  if (!sTokens) return false;
  sTokens--;
  return true;
}

static bool seenRecently(uint64_t sig) {
  if (!sig) return false;                     // nothing to recognise it by
  for (size_t i = 0; i < kInboxRecentSigs; i++) if (sRecent[i] == sig) return true;
  return false;
}

bool note(const uint8_t from[16], uint8_t standing, uint8_t via, double sentAt,
          const char* text, size_t textLen, uint64_t sigPrefix) {
  // Counted, not just refused. A message the node took in and did not keep is
  // a hole in the log, and a hole that reports nothing is indistinguishable
  // from a message that never arrived — which is exactly the shape of an hour
  // spent looking in the wrong place.
  if (!sReady || !sQueue) { sDropped++; return false; }

  // A message the sender sent again because it did not see a proof is the
  // same message. Storing it twice takes a second of the fifty slots and
  // pushes somebody else's out, which over LoRa — where a lost proof is
  // ordinary — costs several slots for one conversation.
  if (seenRecently(sigPrefix)) { sDropped++; return false; }
  if (!takeToken()) {
    sDropped++;
    return false;
  }

  InboxRecord r{};
  r.seq    = 0;                               // assigned when it is written
  r.bootId = sBootId;
  r.bootMs = millis();                        // when it arrived, not when it was stored
  r.sentAt = inboxSentAt(sentAt);
  memcpy(r.from, from, 16);
  r.standing = standing;
  r.via = via;
  // Kept truncated rather than dropped. What the log is for is that something
  // arrived and who from; the whole of a long message is the sender's client's
  // job, not a node's.
  const size_t n = textLen > kInboxTextMax ? kInboxTextMax : textLen;
  r.textLen = (uint16_t)n;
  if (n) memcpy(r.text, text, n);

  // Never waits. This runs inside the packet callback, and a full queue means
  // the loop task is behind — which is a reason to drop a message, not to hold
  // up the Reticulum task until it catches up.
  if (xQueueSend(sQueue, &r, 0) != pdTRUE) {
    sDropped++;
    return false;
  }
  sRecent[sRecentAt] = sigPrefix;
  sRecentAt = (sRecentAt + 1) % kInboxRecentSigs;
  return true;
}

// One queued record onto the store. Everything slow happens here, on the loop
// task, and never on the task that took the message in.
static bool write(const InboxRecord& queued) {
  Sys::Lock held(sLock);
  InboxRecord r = queued;
  r.seq = sNewest + 1;

  uint8_t buf[kInboxRecordSize];
  encodeInbox(r, buf);

  fs::FS& fs = *RnsFileSystem::backend();
  // Opened for update rather than for append: the write lands in the slot the
  // sequence number picks, over whatever was there.
  File f = fs.open(kPath, "r+");
  if (!f) {
    sDropped++;
    log_w("inbox: cannot open %s to store a message", kPath);
    return false;
  }
  const bool ok = f.seek(inboxSlot(r.seq) * kInboxRecordSize) &&
                  f.write(buf, sizeof(buf)) == sizeof(buf);
  f.close();
  if (!ok) {
    sDropped++;
    log_w("inbox: could not store message #%lu", (unsigned long)r.seq);
    return false;
  }
  sNewest = r.seq;
  if (sStored < kInboxSlots) sStored++;
  return true;
}

void poll() {
  if (!sQueue) return;
  InboxRecord r;
  // One per pass. The loop task has a display, a console and a restart
  // sequence to get to, and a burst of messages does not need to be written
  // faster than it comes round again.
  if (xQueueReceive(sQueue, &r, 0) == pdTRUE) write(r);
}

// Reads one record out of an open file. The caller holds the lock.
static bool readAt(File& f, uint32_t seq, InboxRecord& out) {
  uint8_t buf[kInboxRecordSize];
  if (!f.seek(inboxSlot(seq) * kInboxRecordSize)) return false;
  if (f.read(buf, sizeof(buf)) != (int)sizeof(buf)) return false;
  if (!decodeInbox(buf, out)) return false;
  // The slot is right and the record is well formed, but it is only the
  // message that was asked for if the sequence numbers agree — a wrap that
  // happened between the caller reading newest() and reading this record
  // lands here.
  return out.seq == seq;
}

// Whether a sequence number is still inside the ring at all, which is cheaper
// to answer than to find out by reading a slot somebody else has since
// overwritten and returning their message under the number that was asked for.
static bool inRing(uint32_t seq) {
  if (!seq || seq > sNewest) return false;
  return !(sNewest >= kInboxSlots && seq <= sNewest - kInboxSlots);
}

bool read(uint32_t seq, InboxRecord& out) {
  if (!sReady || !inRing(seq)) return false;
  Sys::Lock held(sLock);
  fs::FS& fs = *RnsFileSystem::backend();
  File f = fs.open(kPath, FILE_READ);
  if (!f) return false;
  const bool got = readAt(f, seq, out);
  f.close();
  return got;
}

Page readPage(uint32_t fromSeq, size_t max, void (*fn)(const InboxRecord&, void*), void* ctx) {
  Page page;
  if (!sReady || !fn) return page;
  Sys::Lock held(sLock);
  if (!fromSeq || fromSeq > sNewest) fromSeq = sNewest;
  fs::FS& fs = *RnsFileSystem::backend();
  File f = fs.open(kPath, FILE_READ);
  if (!f) return page;
  uint32_t seq = fromSeq;
  for (; page.count < max && inRing(seq); seq--) {
    InboxRecord r;
    if (!readAt(f, seq, r)) break;
    fn(r, ctx);
    page.count++;
    page.oldest = seq;
    if (seq == 1) break;                     // stop before the unsigned wraps
  }
  // Whether there is another page, answered while the file is still open and
  // the lock still held, so it cannot disagree with what was just read.
  if (page.count && page.oldest > 1) {
    InboxRecord probe;
    page.more = inRing(page.oldest - 1) && readAt(f, page.oldest - 1, probe);
  }
  f.close();
  return page;
}

} // namespace Inbox
} // namespace Rns
