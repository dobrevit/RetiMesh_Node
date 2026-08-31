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
#include <Preferences.h>
#include "Lock.h"

namespace Rns {
namespace Admin {

// One command waiting for the loop task. Deeper than one so a second command
// arriving while the first runs is not lost; shallow because a node that is
// three commands behind has a problem no queue depth fixes.
struct Pending {
  uint8_t from[16];
  char    text[kMaxCommand + 1];
};

static QueueHandle_t     sQueue = nullptr;
// One lock over everything below. Three tasks reach it: the Reticulum task
// judges, the loop task runs and reports, and the web task reloads the list
// when the settings are saved.
static SemaphoreHandle_t sLock = nullptr;
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
// The floor each administrator has reached, kept where authorisation state
// belongs: its own store, written when a command is accepted, read at boot.
// Small and rare — a handful of writes a day on a node anybody administers —
// so NVS carries it without complaint.
// ---------------------------------------------------------------------------
static Preferences sFloors;
static bool        sFloorsOpen = false;

struct FloorRecord { uint8_t hash[16]; uint64_t bits; };   // the timestamp's bit pattern

static void loadFloors(List& into) {
  if (!sFloorsOpen) return;
  FloorRecord recs[kMaxAdmins] = {};
  const size_t got = sFloors.getBytes("floors", recs, sizeof(recs));
  if (got != sizeof(recs)) return;                          // never written, or a different shape
  for (size_t i = 0; i < into.count; i++) {
    for (const FloorRecord& r : recs) {
      if (memcmp(r.hash, into.hash[i], 16) != 0) continue;
      double v = 0;
      memcpy(&v, &r.bits, sizeof(v));
      if (v > into.lastSeen[i]) into.lastSeen[i] = v;
      break;
    }
  }
}

static void saveFloors(const List& from) {
  if (!sFloorsOpen) return;
  FloorRecord recs[kMaxAdmins] = {};
  for (size_t i = 0; i < from.count && i < kMaxAdmins; i++) {
    memcpy(recs[i].hash, from.hash[i], 16);
    memcpy(&recs[i].bits, &from.lastSeen[i], sizeof(recs[i].bits));
  }
  if (sFloors.putBytes("floors", recs, sizeof(recs)) != sizeof(recs))
    log_w("rns admin: could not record how far each administrator has got; a "
          "command already run could be replayed after a restart");
}

// ---------------------------------------------------------------------------

void begin() {
  if (!sQueue) sQueue = xQueueCreate(2, sizeof(Pending));
  if (!sLock)  sLock  = xSemaphoreCreateMutex();
  if (!sFloorsOpen) sFloorsOpen = sFloors.begin("rnsadm", false);
  reload();
}

// Reads the switch and the list, and recovers how far each administrator had
// got. Called again whenever the settings are saved: taking somebody off the
// list is the thing an operator does in a hurry, and "it applies at the next
// restart" is the wrong answer to that.
//
// Built whole and then swapped in under the lock. Assembling it in place
// zeroed every floor first and re-read them afterwards, and a command arriving
// on the Reticulum task inside that window was judged against a floor of zero
// — so a captured command would have passed the one check meant to stop it.
void reload() {
  const MaintenanceSettings& m = settings.maintenance();
  List next;
  const bool parsed = parseAdmins(m.rnsAdmins, next);
  if (!parsed) {
    next = List{};
    log_e("rns admin: the administrator list does not parse; nobody is allowed");
  }
  loadFloors(next);
  // Anything this run has already seen outranks what was stored, in case a
  // command was accepted since the last write.
  {
    Sys::Lock held(sLock);
    for (size_t i = 0; i < next.count; i++) {
      const size_t was = indexOf(sList, next.hash[i]);
      if (was < sList.count && sList.lastSeen[was] > next.lastSeen[i])
        next.lastSeen[i] = sList.lastSeen[was];
    }
    sList = next;
    sEnabled = m.rnsAdmin;
  }
  if (!m.rnsAdmin || !next.count) {
    log_i("rns admin: off%s", m.rnsAdmin && !next.count ? " (switched on, but nobody is listed)" : "");
    return;
  }
  log_i("rns admin: on, %u administrator(s)", (unsigned)next.count);
}

size_t adminCount() { Sys::Lock held(sLock); return sList.count; }

State state() {
  Sys::Lock held(sLock);
  State s;
  s.offered = sOffered;
  s.ran = sRan;
  s.lastVerdict = sLastVerdict;
  strlcpy(s.lastFrom, sLastFrom, sizeof(s.lastFrom));
  s.lastAgoMs = sLastAtMs ? millis() - sLastAtMs : 0;
  return s;
}

// Says no, out loud and to the sender. A refusal that only reaches the log is
// indistinguishable from an unreachable node by the one person who needs to
// tell those apart.
static void refuse(const uint8_t source[16], Verdict v, const char* from) {
  log_w("rns admin: refused a command from %s: %s", from, verdictName(v));
  char why[96];
  snprintf(why, sizeof(why), "RM ERR ADMIN 403 %s", verdictName(v));
  // Not to a sender this node cannot even name: telling a stranger which of
  // the questions they failed is telling them how to pass it, and an
  // unverified source hash is a claim, so the answer would go to whoever the
  // claim named rather than to whoever sent it.
  if (v != NotVerified && v != NotAdmin && v != Disabled)
    RnsTransport::queueLxmfReply(source, why);
}

bool offer(const uint8_t source[16], uint8_t standing, double sentAt,
           const char* text, size_t textLen) {
  // Nothing is recorded for a node with the feature off. A counter that ticks
  // for every message anybody sends would say a node was being probed when it
  // was only being messaged.
  {
    Sys::Lock held(sLock);
    if (!sEnabled || !sList.count) return false;
  }

  Caller who;
  who.standing = standing;
  who.source = source;
  who.sentAt = sentAt;
  who.textLen = textLen;

  char from[33];
  toHex(source, 16, from);

  Sys::Lock held(sLock);
  size_t which = kMaxAdmins;
  Verdict v = judge(sEnabled, sList, who, StandingVerified, which);
  // The console the command would run on. Asked here so the answer is a
  // refusal with a reason rather than a command that appears to run and
  // returns an empty reply — which is what happened when the console was off,
  // because the parser silently discards everything it is fed.
  if (v == Allowed && !settings.maintenance().consoleEnabled) v = NoConsole;

  sOffered++;
  sLastVerdict = (uint8_t)v;
  sLastAtMs = millis();
  strlcpy(sLastFrom, from, sizeof(sLastFrom));

  if (v != Allowed) {
    held.release();
    refuse(source, v, from);
    return false;
  }

  Pending p{};
  memcpy(p.from, source, 16);
  memcpy(p.text, text, textLen);
  p.text[textLen] = '\0';
  if (!sQueue || xQueueSend(sQueue, &p, 0) != pdTRUE) {
    // The floor is deliberately not moved. A command that never ran must stay
    // runnable: raising it here meant an administrator whose command was
    // dropped had to invent a newer one, with nothing to tell them why the
    // last was ignored.
    held.release();
    log_w("rns admin: a command from %s arrived while the last one was still running", from);
    RnsTransport::queueLxmfReply(source, "RM ERR ADMIN 503 busy; send it again");
    return false;
  }
  // Moved on once the command is certain to run, and recorded before it does.
  // Two copies of one message can arrive close together, and the second must
  // be refused whether or not the loop task has got to the first.
  sList.lastSeen[which] = sentAt;
  saveFloors(sList);
  return true;
}

// Closes the session on every way out of poll(), including the one that leaves
// through an exception. The stream is on this task's stack and the console
// holds a pointer to it in a static table; a throw between opening and closing
// would leave that pointer aimed at a frame that no longer exists, and the very
// next pass reads through it.
class SessionHold {
public:
  SessionHold(Stream& io, bool ok) : _io(io), _ok(ok) {}
  ~SessionHold() { if (_ok) Maintenance::closeSession(_io); }
  SessionHold(const SessionHold&) = delete;
  SessionHold& operator=(const SessionHold&) = delete;
private:
  Stream& _io;
  bool    _ok;
};

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
  const bool opened = Maintenance::openSession(stream, /*hostFacing*/ false, /*preAuthed*/ true);
  SessionHold hold(stream, opened);
  if (!opened) {
    log_w("rns admin: no console session free for %s", from);
    RnsTransport::queueLxmfReply(p.from, "RM ERR ADMIN 503 no console session free; send it again");
    return;
  }
  stream.feed(p.text);
  Maintenance::poll();                         // reads the line, writes the reply into the stream
  {
    Sys::Lock held(sLock);
    sRan++;
  }

  // Room for the note as well as the answer. It went in the same buffer as
  // the answer before, so the one time it was needed it was itself cut off
  // half way through — a truncation warning that gets truncated is worse than
  // none, because it reads as part of the reply.
  char reply[256];
  snprintf(reply, sizeof(reply), "%s%s", stream.out(),
           stream.clipped() ? "\n(reply truncated; ask for less at once)" : "");
  if (!RnsTransport::queueLxmfReply(p.from, reply))
    log_w("rns admin: ran the command for %s but could not answer", from);
}

} // namespace Admin
} // namespace Rns
