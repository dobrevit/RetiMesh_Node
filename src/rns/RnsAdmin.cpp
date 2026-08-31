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
//  RnsAdmin.cpp — the gate on a node, and the console behind it
//
//  The judgement is in the header and argued with on a host. What is here is
//  everything that needs a node: where the list comes from, which task does
//  what, and how a reply gets back.
//
//  Two tasks, for the reason the inbox has two: a message arrives on the
//  Reticulum task, and running a command means the console's parser, the
//  settings, possibly a restart — none of which belong inside a packet
//  callback. So the Reticulum task only judges, which is arithmetic over
//  values it already has, and the loop task runs what passed.
// ============================================================================
#include "RnsAdmin.h"
#include "RnsAnnounce.h"        // toHex
#include "RnsTransport.h"
#include "LxmfInbox.h"
#include "Maintenance.h"
#include "Settings.h"
#include <Arduino.h>
#include <freertos/queue.h>

namespace Rns {
namespace Admin {

// One command waiting for the loop task. Deeper than one so a second command
// arriving while the first runs is not lost; shallow because a node that is
// three commands behind has a problem no queue depth fixes.
struct Pending {
  uint8_t from[16];
  char    text[kMaxCommand + 1];
};

static QueueHandle_t sQueue = nullptr;
static List          sList;
static bool          sEnabled = false;
static uint32_t      sOffered = 0, sRan = 0;
static uint8_t       sLastVerdict = Disabled;
static char          sLastFrom[33] = "";
static uint32_t      sLastAtMs = 0;

// ---------------------------------------------------------------------------
// A command in, a reply out, wearing the shape the console already reads.
//
// The console is written against a Stream and does not care what is on the
// other end of one — a UART, a socket, or this. That is the whole reason the
// command language is not written twice: what an administrator may type into
// a message is exactly what they may type at the cable, checked by the same
// parser, refused by the same rules.
// ---------------------------------------------------------------------------
class MessageStream : public Stream {
public:
  void feed(const char* line) {
    _inLen = 0;
    while (*line && _inLen < sizeof(_in) - 2) _in[_inLen++] = *line++;
    _in[_inLen++] = '\n';                      // the parser answers complete lines
    _inAt = 0;
  }

  int available() override { return (int)(_inLen - _inAt); }
  int read() override      { return _inAt < _inLen ? (uint8_t)_in[_inAt++] : -1; }
  int peek() override      { return _inAt < _inLen ? (uint8_t)_in[_inAt] : -1; }
  void flush() override    {}

  size_t write(uint8_t c) override { return write(&c, 1); }

  // Bounded, and silent about it. What does not fit is what the reply says it
  // dropped: a message has an MTU, and a STATUS answer is longer than one.
  size_t write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; i++) {
      if (_outLen >= sizeof(_out) - 1) { _clipped = true; break; }
      _out[_outLen++] = (char)data[i];
    }
    return len;                                // never a short count: see ConsoleServer.h
  }

  const char* out() { _out[_outLen] = '\0'; return _out; }
  bool clipped() const { return _clipped; }

private:
  char   _in[kMaxCommand + 2] = "";
  size_t _inLen = 0, _inAt = 0;
  // Enough for a several-line answer, and short enough that the whole reply —
  // this, plus the note saying it was cut, plus the eighty bytes of envelope
  // and the payload's own framing — still fits in one packet.
  char   _out[200] = "";
  size_t _outLen = 0;
  bool   _clipped = false;
};

// ---------------------------------------------------------------------------

void begin() {
  if (!sQueue) sQueue = xQueueCreate(2, sizeof(Pending));
  reload();
}

// Reads the switch and the list, and works out where each administrator got
// to. Called again whenever the settings are saved: taking somebody off the
// list is the thing an operator does in a hurry, and "it applies at the next
// restart" is the wrong answer to that.
void reload() {
  const MaintenanceSettings& m = settings.maintenance();
  sEnabled = m.rnsAdmin;
  if (!parseAdmins(m.rnsAdmins, sList)) {
    sList = List{};
    log_e("rns admin: the administrator list does not parse; nobody is allowed");
  }
  if (!sEnabled || !sList.count) {
    log_i("rns admin: off%s", sEnabled && !sList.count ? " (switched on, but nobody is listed)" : "");
    return;
  }
  // Where each administrator got to before the restart. Without this a
  // command captured off the air could be sent again after any reboot and
  // would look new — the node would have forgotten it had already run it.
  // The inbox has every message it stored, with the sender's clock on each,
  // so the floor is there to be read rather than kept a second time.
  const uint32_t newest = Inbox::newest();
  size_t scanned = 0;
  for (uint32_t seq = newest; seq >= 1 && scanned < kInboxSlots; seq--, scanned++) {
    InboxRecord r;
    if (!Inbox::read(seq, r)) break;
    const size_t i = indexOf(sList, r.from);
    if (i < sList.count && r.sentAt > sList.lastSeen[i]) sList.lastSeen[i] = r.sentAt;
    if (seq == 1) break;
  }
  log_i("rns admin: on, %u administrator(s)", (unsigned)sList.count);
}

size_t adminCount() { return sList.count; }

State state() {
  State s;
  s.offered = sOffered;
  s.ran = sRan;
  s.lastVerdict = sLastVerdict;
  strlcpy(s.lastFrom, sLastFrom, sizeof(s.lastFrom));
  s.lastAgoMs = sLastAtMs ? millis() - sLastAtMs : 0;
  return s;
}

bool offer(const uint8_t source[16], uint8_t standing, double sentAt,
           const char* text, size_t textLen) {
  // Nothing is recorded for a node with the feature off. A counter that ticks
  // for every message anybody sends would say a node was being probed when it
  // was only being messaged.
  if (!sEnabled || !sList.count) return false;

  Caller who;
  who.standing = standing;
  who.source = source;
  who.sentAt = sentAt;
  who.textLen = textLen;
  size_t which = kMaxAdmins;
  const Verdict v = judge(sEnabled, sList, who, StandingVerified, which);

  sOffered++;
  sLastVerdict = (uint8_t)v;
  sLastAtMs = millis();
  toHex(source, 16, sLastFrom);

  if (v != Allowed) {
    // Said plainly and at warning level. Somebody trying the door is the one
    // thing on this path an operator wants in the log without asking for it.
    log_w("rns admin: refused a command from %s: %s", sLastFrom, verdictName(v));
    return false;
  }

  // Moved on before the command has run, not after. Two copies of one message
  // can arrive close together, and the second must be refused whether or not
  // the loop task has got to the first yet.
  sList.lastSeen[which] = sentAt;

  Pending p{};
  memcpy(p.from, source, 16);
  const size_t n = textLen > kMaxCommand ? kMaxCommand : textLen;
  memcpy(p.text, text, n);
  p.text[n] = '\0';
  if (!sQueue || xQueueSend(sQueue, &p, 0) != pdTRUE) {
    log_w("rns admin: a command from %s arrived while the last one was still running", sLastFrom);
    return false;
  }
  return true;
}

void poll() {
  if (!sQueue) return;
  Pending p;
  if (xQueueReceive(sQueue, &p, 0) != pdTRUE) return;

  char from[33];
  toHex(p.from, 16, from);
  log_i("rns admin: running \"%s\" for %s", p.text, from);

  MessageStream stream;
  // Not host-facing, deliberately. It is the same answer the console gives a
  // caller out on the station network, and it means the bootloader declines
  // over the air without a second rule being written for it: a node put into
  // its ROM downloader by a message is a node nobody can reach at all.
  if (!Maintenance::openSession(stream, /*hostFacing*/ false, /*preAuthed*/ true)) {
    log_w("rns admin: no console session free for %s", from);
    return;
  }
  stream.feed(p.text);
  Maintenance::poll();                         // reads the line, writes the reply into the stream
  Maintenance::closeSession(stream);
  sRan++;

  // Room for the note as well as the answer. It went in the same buffer as
  // the answer before, so the one time it was needed it was itself cut off
  // half way through — a truncation warning that gets truncated is worse than
  // none, because it reads as part of the reply.
  char reply[256];
  snprintf(reply, sizeof(reply), "%s%s", stream.out(),
           stream.clipped() ? "\n(reply truncated; ask for less at once)" : "");
  if (!RnsTransport::sendLxmf(p.from, reply))
    log_w("rns admin: ran the command for %s but could not answer", from);
}

} // namespace Admin
} // namespace Rns
