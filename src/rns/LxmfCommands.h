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
//  LxmfCommands.h — answering the three questions a client actually asks
//
//  Sideband has buttons for these, and every LXMF client that follows it sends
//  the same three: are you there, say this back to me, and how well did you
//  hear me. They arrive in a message's fields map (LxmfFormat.h) and the
//  answer is an ordinary message going the other way — no new protocol, no new
//  destination, nothing to configure on the phone.
//
//  They are worth answering because of what a node is for. A person walking a
//  valley with a phone wants to know where the node stops hearing them, and
//  the only honest answer to that is the one the node gives from where it is.
//  Until now the node could be messaged and would say nothing back, so the
//  only way to test coverage was two people and two radios.
//
//  These are emphatically not administration. They tell a stranger nothing
//  they did not already know — that the node exists, which they learned by
//  reaching it, and how strong their own signal was, which their own radio
//  could tell them. Commands that *change* something go through RnsAdmin.h,
//  behind a list, a signature and a replay floor. The two are kept apart on
//  purpose: this file must never grow a command that does anything.
//
//  What it costs is airtime, which on LoRa is the scarce thing. A reply is one
//  short packet for one short packet — no amplification — and the caller holds
//  a cooldown so one peer cannot monopolise the radio. The duty-cycle
//  accounting a node already keeps (Airtime.h) is the backstop.
//
//  Pure, so the wording and the arithmetic can be argued with on a host: what
//  goes out over the air on a stranger's say-so is worth a test.
// ============================================================================
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "LxmfFormat.h"

namespace Rns {
namespace Commands {

// What the radio measured of the packet that carried the command. Not a
// number this node chooses — it is the receiver's own reading, which is
// precisely what makes it worth sending back. NaN where the path had none to
// give: a message that came in over Wi-Fi or a link has no RSSI, and saying so
// is better than inventing a zero.
struct Signal {
  float rssi = NAN;
  float snr  = NAN;
  float q    = NAN;
};

// Longest reply this file produces: three signal lines with room to spare.
static const size_t kReplyMax = 96;

// The answer to one command, or nothing.
//
// Returns the length written, and 0 when this node does not answer that
// command — an unknown command, or a telemetry request, which is a real
// question this node cannot answer yet and should not pretend to. Answering
// "no" to it would put a confusing message in somebody's conversation; saying
// nothing leaves their client showing the request unanswered, which is true.
//
// The wording matches what Sideband itself replies, so a node's answer reads
// the same as another person's phone would.
inline size_t reply(const LxmfCommand& c, const Signal& s, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';

  if (c.id == kCommandPing) {
    if (cap < 11) return 0;
    memcpy(out, "Ping reply", 11);
    return 10;
  }

  if (c.id == kCommandEcho) {
    // The sender's own bytes, back. They chose them, so there is nothing here
    // to leak — but they arrive from a stranger and go out over this node's
    // radio, so what is echoed is trimmed to a character boundary and capped.
    // A body that is not text is not echoed at all rather than reflected raw.
    static const char* kPrefix = "Echo reply: ";
    const size_t plen = strlen(kPrefix);
    if (cap <= plen + 1) return 0;
    const size_t room = cap - plen - 1;
    const size_t n = utf8TrimLen(c.text, c.textLen, room < 64 ? room : 64);
    memcpy(out, kPrefix, plen);
    if (n) memcpy(out + plen, c.text, n);
    out[plen + n] = '\0';
    return plen + n;
  }

  if (c.id == kCommandSignal) {
    // Sideband's own layout, line for line, so the reply reads the way its
    // users already expect. A line whose figure this node does not have is
    // left out rather than filled in.
    size_t i = 0;
    auto line = [&](const char* fmt, float v) {
      if (isnan(v) || i >= cap) return;
      const int k = snprintf(out + i, cap - i, fmt, (double)v);
      if (k > 0 && (size_t)k < cap - i) i += (size_t)k;
    };
    line("Link Quality: %.0f%%\n", s.q);
    line("RSSI: %.0f dBm\n", s.rssi);
    line("SNR: %.1f dB\n", s.snr);
    if (i == 0) {
      static const char* none = "No reception info available";
      const size_t n = strlen(none);
      if (cap <= n) return 0;
      memcpy(out, none, n + 1);
      return n;
    }
    out[--i] = '\0';                    // the trailing newline, as Sideband trims it
    return i;
  }

  return 0;                             // including a telemetry request, for now
}

} // namespace Commands
} // namespace Rns
