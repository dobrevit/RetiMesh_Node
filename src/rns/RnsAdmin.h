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
//  RnsAdmin.h — administering a node by messaging it, and who may
//
//  A node whose cable dies is not reachable at all. That is not a hypothetical:
//  a native-USB board can come back from a flash with its USB unit enumerated
//  and dead, running perfectly, answering nothing — and the only way back is
//  somebody walking to it and pulling the plug. Meanwhile the node is still
//  there on the mesh, announcing, one hop away, with a delivery address that
//  works.
//
//  So: a message can carry a command. The whole difficulty is that the
//  delivery address is reachable by anyone who can route to it — over LoRa,
//  over the node's own TCP interface, over AutoInterface on the LAN — and none
//  of those authenticate. "Anyone on the network can speak to the nodes" is
//  the correct description of the starting position, and the answer is four
//  separate questions, asked in order, each of which can only refuse:
//
//    1. Is remote administration switched on, and is anybody allowed?
//       Off by default, and an empty list is off however the switch reads.
//       A feature that can turn a node inside out does not arrive enabled.
//
//    2. Is the sender proved to be who they claim?
//       Only `verified` — this node has heard them announce, holds their key,
//       and the signature over this message matched it. Not `unverified`,
//       which means there was no key to check, and emphatically not
//       `mismatch`. This is the standing the inbox has been recording since
//       messages were first stored, kept apart from the other two for exactly
//       this moment (LxmfInbox.h).
//
//    3. Is that identity one this node takes orders from?
//       Verification proves *who*, never *what they may do* — anyone at all
//       can announce, and a proved stranger is still a stranger. The list is
//       the authorisation, and it is configured on the node by somebody who
//       already has the cable or the portal.
//
//    4. Is this message new?
//       A signed message is a signed message for ever, so one captured off
//       the air can be sent again by anyone who kept a copy — they do not
//       need the key, only the bytes. Each admin's commands must carry a
//       strictly increasing timestamp, which their own clock provides and an
//       attacker replaying an old message cannot.
//
//       The floor each administrator has reached is kept on its own terms, in
//       its own store, written when a command is accepted. It was derived
//       from the inbox at first, and that was wrong twice over: the inbox
//       stores what *anybody* sends, so a stranger could put a record dated
//       2096 under an administrator's source hash and lock the real one out
//       until then; and the inbox is a fifty-slot ring, so an attacker who
//       filled it could erase the evidence of a command and replay it after
//       the next restart. A floor that authorises has to be authorisation
//       state, not a by-product of a message log anybody can write to.
//
//  What a command may then do is not decided here. It goes to the same parser
//  the console uses, as a session that is not host-facing — so the vocabulary,
//  the argument checking and the refusals are the ones already written and
//  already tested, and the bootloader declines for the same reason it declines
//  from the station network. One language, not two (Maintenance.h).
//
//  The judgement is a pure function so it can be argued with on a host. That
//  matters more here than anywhere else in this firmware: it is the only code
//  where being wrong hands a stranger the node.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Rns {
namespace Admin {

// Four is more administrators than a node has and few enough to read off a
// settings page. The list is stored as hex, comma separated.
static const size_t kMaxAdmins = 4;

// Long enough for the longest command the console accepts (MAX_LINE), which
// is what decides this rather than anything about messages.
static const size_t kMaxCommand = 96;

// Every way this can end. Refusals are named rather than collapsed into a
// bool because an operator whose command did nothing needs to know which of
// the four questions it failed, and because a node that says "not on the
// list" to a stranger has told them nothing they did not already know.
enum Verdict : uint8_t {
  Allowed = 0,
  Disabled,        // switched off, or nobody is listed
  NotVerified,     // no key to check against, or the signature did not match
  NotAdmin,        // proved, but not somebody this node takes orders from
  Replayed,        // not newer than the last command accepted from them
  Empty,           // nothing to run
  TooLong,         // longer than a command line, so what ran would not be what was sent
  NoConsole,       // the console this would run on is switched off
};

inline const char* verdictName(Verdict v) {
  return v == Allowed     ? "allowed"
       : v == Disabled    ? "disabled"
       : v == NotVerified ? "unverified sender"
       : v == NotAdmin    ? "not an administrator"
       : v == Replayed    ? "replayed"
       : v == Empty       ? "empty command"
       : v == TooLong     ? "command too long"
       : v == NoConsole   ? "the console is switched off"
                          : "unknown";
}

// The list, parsed. Kept as bytes rather than re-parsed per message, and
// carried into the judgement so the judgement itself touches no globals.
struct List {
  uint8_t  hash[kMaxAdmins][16];
  double   lastSeen[kMaxAdmins];      // newest command timestamp accepted from each
  size_t   count = 0;
};

// One hex pair. Returns false for anything that is not one, which is what
// keeps a typo in a settings field from becoming a silent zero byte.
inline bool hexByte(const char* p, uint8_t& out) {
  uint8_t v = 0;
  for (int i = 0; i < 2; i++) {
    const char c = p[i];
    v <<= 4;
    if      (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint8_t)(c - 'A' + 10);
    else return false;
  }
  out = v;
  return true;
}

// "aabb…,ccdd…" into the list. Anything malformed is refused whole rather
// than partly accepted: a list that silently dropped the half it could not
// read would be a shorter list than the operator believes they configured,
// and the failure would be invisible until the day it mattered.
inline bool parseAdmins(const char* text, List& out) {
  out = List{};
  if (!text || !*text) return true;                  // empty is a valid empty list
  const char* p = text;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    if (out.count >= kMaxAdmins) return false;       // more than the node will hold
    for (size_t b = 0; b < 16; b++) {
      if (!hexByte(p + b * 2, out.hash[out.count][b])) return false;
    }
    p += 32;
    out.count++;
    if (*p && *p != ',' && *p != ' ') return false;  // 32 hex digits, then a separator
  }
  return true;
}

// Which entry this source is, or kMaxAdmins for none.
inline size_t indexOf(const List& list, const uint8_t source[16]) {
  for (size_t i = 0; i < list.count; i++)
    if (memcmp(list.hash[i], source, 16) == 0) return i;
  return kMaxAdmins;
}

// What arrived, as much of it as the judgement needs.
struct Caller {
  uint8_t        standing = 0;        // InboxStanding: only StandingVerified passes
  const uint8_t* source = nullptr;    // 16 bytes
  double         sentAt = 0;          // the sender's clock, from the message
  size_t         textLen = 0;         // how much command there is
};

// The four questions, in order. `verifiedStanding` is passed in rather than
// included so this header does not have to depend on the inbox's enum for one
// number; the caller hands it StandingVerified.
inline Verdict judge(bool enabled, const List& list, const Caller& who,
                     uint8_t verifiedStanding, size_t& which) {
  which = kMaxAdmins;
  if (!enabled || list.count == 0) return Disabled;
  if (!who.source) return NotAdmin;
  // Before anything about who they are: whether this node can tell. An
  // unverified sender is not a suspicious one, it is an unknown one, and a
  // command from an unknown sender is not a command.
  if (who.standing != verifiedStanding) return NotVerified;
  const size_t i = indexOf(list, who.source);
  if (i >= list.count) return NotAdmin;
  // Strictly newer. Equal is refused too: the same message sent twice is the
  // same message, and a clock that does not move between two commands cannot
  // distinguish a repeat from a replay.
  if (!(who.sentAt > list.lastSeen[i])) return Replayed;
  if (who.textLen == 0) return Empty;
  // Refused rather than cut down to fit. The console's line limit is the same
  // number, so a command trimmed to it parses perfectly and runs — which means
  // "SET admin.password <a long passphrase>" would set a password neither the
  // sender nor the operator knows, and answer OK. What runs must be what was
  // sent, or nothing.
  if (who.textLen > kMaxCommand) return TooLong;
  which = i;
  return Allowed;
}

// ---------------------------------------------------------------------------
// The rest needs a node under it.
// ---------------------------------------------------------------------------

// Reads the list out of settings and seeds each administrator's last-seen
// timestamp from the newest command already stored in the inbox, so a restart
// does not reopen the window on messages this node has already run.
void begin();

// Re-reads the switch and the list. The settings commit calls it, so a change
// applies at once rather than at the next restart — which matters most for the
// change somebody makes in a hurry, which is taking a name off the list.
void reload();

// A message arrived and was taken. Returns whether it was run, and says why
// not in the log when it was not. `text` is the message content, which is the
// command line; the reply goes back to the sender as a message.
bool offer(const uint8_t source[16], uint8_t standing, double sentAt,
           const char* text, size_t textLen);

// Runs one accepted command, on the loop task. A command means the console's
// parser, the settings, and sometimes a restart, none of which belong inside
// the packet callback the message arrived in.
void poll();

// What the last offer came to, for the console and the page: an operator
// switching this on wants to see that their command was refused and which
// question it failed.
struct State {
  uint32_t offered = 0;              // commands seen
  uint32_t ran = 0;                  // ...of which this many were run
  uint8_t  lastVerdict = Disabled;
  char     lastFrom[33] = "";
  uint32_t lastAgoMs = 0;
};
State state();

// How many administrators are listed, for a status line that would otherwise
// report the feature as "on" on a node where nobody can use it.
size_t adminCount();

} // namespace Admin
} // namespace Rns
