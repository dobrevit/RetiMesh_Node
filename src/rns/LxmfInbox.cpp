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
//  What is here is the file: where it lives, and the lock that lets a message
//  arriving on the Reticulum task and a page being served on the network task
//  touch it at the same time without either seeing half a record.
// ============================================================================
#include "LxmfInbox.h"
#include "RnsFileSystem.h"
#include "Lock.h"
#include <esp_random.h>
#include <Arduino.h>

namespace Rns {
namespace Inbox {

// Beside the transport's own store, under the same root, so it follows the
// store between LittleFS and the card without a second decision about where
// storage lives.
static const char* kPath = RNS_FS_ROOT "/inbox.log";

static SemaphoreHandle_t sLock = nullptr;
static uint32_t sNewest = 0;         // highest sequence number stored; 0 when empty
static uint32_t sStored = 0;         // how many slots hold a message
static uint32_t sBootId = 0;
static bool     sReady  = false;

uint32_t bootId() { return sBootId; }

// The file is created full-size on first use so that every later write is an
// overwrite of one record. Growing it a record at a time would work, but it
// would mean the wrap is the first time the file reaches its final size —
// and the wrap is the case least likely to be exercised before a release.
static bool ensureFile(fs::FS& fs) {
  if (fs.exists(kPath)) return true;
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
  if (!ok) log_e("inbox: could not lay out %s; the store may be full", kPath);
  return ok;
}

void begin() {
  if (!sLock) sLock = xSemaphoreCreateMutex();
  // Distinguishes this run from the last one. A message's "four minutes ago"
  // is only true within the run that took it in, because millis() starts
  // again at every restart; anything older than the current run is shown by
  // the sender's own clock instead.
  sBootId = esp_random();
  if (!sBootId) sBootId = 1;                  // 0 would read as "no boot recorded"

  Sys::Lock held(sLock);
  fs::FS& fs = *RnsFileSystem::backend();
  if (!ensureFile(fs)) return;

  // Where the log left off. Only the sequence number of each slot is read —
  // fifty four-byte reads rather than the whole ten kilobytes — because the
  // rest of a record is not needed to know which one is newest.
  File f = fs.open(kPath, FILE_READ);
  if (!f) return;
  for (size_t slot = 0; slot < kInboxSlots; slot++) {
    if (!f.seek(slot * kInboxRecordSize)) break;
    uint8_t b[4];
    if (f.read(b, 4) != 4) break;
    const uint32_t seq = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (!seq) continue;
    sStored++;
    if (seq > sNewest) sNewest = seq;
  }
  f.close();
  sReady = true;
  log_i("inbox: %lu message(s) kept on %s, newest #%lu",
        (unsigned long)sStored, RnsFileSystem::backendName(), (unsigned long)sNewest);
}

uint32_t stored() { return sStored; }
uint32_t newest() { return sNewest; }

bool append(const uint8_t from[16], uint8_t standing, uint8_t via,
            double sentAt, const char* text, size_t textLen) {
  if (!sReady) return false;
  Sys::Lock held(sLock);

  InboxRecord r{};
  r.seq    = sNewest + 1;
  r.bootId = sBootId;
  r.bootMs = millis();
  r.sentAt = sentAt;
  memcpy(r.from, from, 16);
  r.standing = standing;
  r.via = via;
  // Kept truncated rather than dropped. What the log is for is that something
  // arrived and who from; the whole of a long message is the sender's client's
  // job, not a node's.
  const size_t n = textLen > kInboxTextMax ? kInboxTextMax : textLen;
  r.textLen = (uint16_t)n;
  if (n) memcpy(r.text, text, n);

  uint8_t buf[kInboxRecordSize];
  encodeInbox(r, buf);

  fs::FS& fs = *RnsFileSystem::backend();
  // Opened for update rather than for append: the write lands in the slot the
  // sequence number picks, over whatever was there.
  File f = fs.open(kPath, "r+");
  if (!f) {
    log_w("inbox: cannot open %s to store a message", kPath);
    return false;
  }
  const bool ok = f.seek(inboxSlot(r.seq) * kInboxRecordSize) &&
                  f.write(buf, sizeof(buf)) == sizeof(buf);
  f.close();
  if (!ok) {
    log_w("inbox: could not store message #%lu", (unsigned long)r.seq);
    return false;
  }
  sNewest = r.seq;
  if (sStored < kInboxSlots) sStored++;
  return true;
}

bool read(uint32_t seq, InboxRecord& out) {
  if (!sReady || !seq || seq > sNewest) return false;
  // Older than the ring holds. Said here rather than found out by reading a
  // slot that has since been overwritten and returning somebody else's
  // message under the sequence number that was asked for.
  if (sNewest >= kInboxSlots && seq <= sNewest - kInboxSlots) return false;

  Sys::Lock held(sLock);
  fs::FS& fs = *RnsFileSystem::backend();
  File f = fs.open(kPath, FILE_READ);
  if (!f) return false;
  uint8_t buf[kInboxRecordSize];
  const bool got = f.seek(inboxSlot(seq) * kInboxRecordSize) &&
                   f.read(buf, sizeof(buf)) == (int)sizeof(buf);
  f.close();
  if (!got || !decodeInbox(buf, out)) return false;
  // The slot is right and the record is well formed, but it is only the
  // message that was asked for if the sequence numbers agree — a wrap that
  // happened between the caller reading newest() and reading this record
  // lands here.
  return out.seq == seq;
}

} // namespace Inbox
} // namespace Rns
