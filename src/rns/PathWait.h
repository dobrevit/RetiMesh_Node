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
//  PathWait.h — what to do with a message whose destination has no path yet
//
//  Reticulum will not send to a destination it has no route to, and it says so
//  by refusing rather than by failing loudly. This firmware never asked: neither
//  `Transport::has_path` nor `Transport::request_path` appeared anywhere in
//  src/, so `sendLxmf()` recalled an identity, built a destination, called
//  `packet.send()` and watched it go nowhere. One `log_w`, one message spent,
//  no second attempt.
//
//  That is not an edge case. A path expires; the announce that would refresh
//  it comes at whatever interval the peer chose, and the shipped maximum is
//  twelve hours. For most of the day, most peers this node has *heard* are
//  peers it cannot currently reach — and the only thing between them is a path
//  request nobody was sending.
//
//  Why this is a policy header rather than three lines at the call site
//  -------------------------------------------------------------------
//  RNS's own reference implementation of this is a blocking loop:
//
//      if Transport.has_path(dst): return True
//      else: Transport.request_path(dst)
//      while not Transport.has_path(dst) and time.time() < timeout: sleep(0.05)
//
//  (Transport.cpp carries it as a comment.) That cannot be copied here. The
//  wait happens on the RNS task, which is the task that also runs
//  `reticulum.loop()` — the loop that receives the very path reply being waited
//  for. Sleeping in it would deadlock the answer it is waiting on, and the
//  brief forbids blocking that pass regardless.
//
//  So the wait is turned inside out: no loop, just a decision made once per
//  pass from the clock and two flags. That decision is pure, which is the only
//  reason its wrap-around and its boundaries can be tested at all — none of the
//  states below are reachable from a host test through the transport.
//
//  The numbers are Reticulum's, not ours
//  -------------------------------------
//  Both come from microReticulum's Type.h so a peer and this node agree about
//  what a reasonable wait is, and so there is one place to change if the bench
//  says a slow LoRa link needs longer:
//
//    PATH_REQUEST_TIMEOUT = 15 s   how long a client waits for a path
//    PATH_REQUEST_MI      = 20 s   the floor between automated requests
//
//  The second is airtime manners rather than correctness. A path request is a
//  broadcast every neighbour repeats; a node that re-asks on a tight loop
//  because a peer is simply switched off spends the channel on its own
//  impatience. Note it is *longer* than the wait: one message gets one request,
//  and a second message to the same dead destination waits without asking
//  again.
// ============================================================================

#pragma once

#include <stdint.h>

namespace Rns {

// RNS Type.h: PATH_REQUEST_TIMEOUT, PATH_REQUEST_MI.
constexpr uint32_t kPathWaitMs            = 15000;
constexpr uint32_t kPathRequestMinIntervalMs = 20000;

// Never asked for this destination. Not zero — zero is "asked at millis() 0",
// which happens once per boot and would suppress the first request of the run.
constexpr uint32_t kNeverRequested = UINT32_MAX;

enum class PathAction : uint8_t {
  Send,     // there is a path; the message can go
  Request,  // no path, none asked for recently: ask, and keep waiting
  Wait,     // asked, or asked too recently to ask again; still inside the window
  GiveUp,   // the window closed with no path
};

// What one waiting message should do this pass.
//
// `startedMs` is when the message began waiting, not when the request went
// out, and the bound is measured from it. That matters for the case where the
// manners floor refuses the request: the message would otherwise wait with no
// deadline running at all, because the deadline had been keyed to a request
// that was never made.
struct PathWait {
  uint32_t startedMs = 0;
  bool     asked     = false;
  uint32_t askedMs   = 0;      // kept for the log; the bound does not use it
};

// `sinceLastRequestMs` is the age of the last path request for *this
// destination*, or kNeverRequested. Unsigned throughout: millis() wraps every
// 49 days and a message in flight across the wrap must not be read as having
// waited a month.
//
// Signed subtraction would in fact behave identically for every elapsed time
// this can see — the two only diverge past 2^31 ms, about 24.8 days, and the
// bound here is fifteen seconds. It is written unsigned anyway because that is
// what makes the wrap correct by construction rather than by argument, and
// because the one case where the two genuinely differ — `nowMs` behind
// `startedMs`, where unsigned reads a huge elapsed time and gives up — is
// prevented not by the cast but by both values coming from millis() on the one
// task, in order. A test cannot tell these two apart without inventing a
// month-long wait, so this note is the record instead.
inline PathAction pathAction(bool hasPath,
                             const PathWait& w,
                             uint32_t nowMs,
                             uint32_t sinceLastRequestMs,
                             uint32_t waitMs = kPathWaitMs,
                             uint32_t minIntervalMs = kPathRequestMinIntervalMs) {
  // Asked first, and answered first: a path that arrived while the message was
  // waiting is the whole point, and it is worth sending on even if the wait
  // has technically just run out.
  if (hasPath) return PathAction::Send;
  if ((uint32_t)(nowMs - w.startedMs) >= waitMs) return PathAction::GiveUp;
  if (!w.asked && sinceLastRequestMs >= minIntervalMs) return PathAction::Request;
  return PathAction::Wait;
}

// Why a message that never left is being reported as failed. Kept beside the
// policy because the outbound log, the console and the API must all give the
// same answer, and "it failed" without one is what this feature exists to stop.
enum class SendFailure : uint8_t {
  None = 0,
  NoPath,        // no route to the destination, and asking for one timed out
  NoKey,         // the destination's identity was never learned
  Refused,       // the transport would not take the packet
};

inline const char* failureName(SendFailure f) {
  switch (f) {
    case SendFailure::NoPath:  return "no path";
    case SendFailure::NoKey:   return "no key";
    case SendFailure::Refused: return "refused";
    default:                   return "";
  }
}

} // namespace Rns
