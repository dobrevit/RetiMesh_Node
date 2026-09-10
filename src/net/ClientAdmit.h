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
//  ClientAdmit.h — whether a peer on :4242 gets a slot, and who frees what
//                  when it does not
//
//  Every socket that arrives at RetiTransportServer::onClient() costs a
//  per-client context before it is worth anything: an id, the AsyncClient*,
//  and an HDLC::Deframer, whose buffer is `uint8_t buf[RNS_MTU]`
//  (RetiTransportServer.h's ClientCtx, HDLC.h's Deframer). That buffer, plus
//  the deframer's length, drop count and three flags, rounds to 512 B at the
//  target's four-byte alignment, and the id and the client pointer in front of
//  it make 520 — one contiguous block of byte-addressable RAM, per peer. The
//  MTU it is sized from is Config.h's; the figures are worked out from it
//  rather than restated, and the host's are larger again because a 64-bit host
//  widens both the length and the pointer, which is why the host test pins the
//  relationship and not the number.
//
//  That allocation happens on the AsyncTCP event task, where nothing catches
//  anything. Diag::begin() installs std::set_terminate (src/sys/Diag.cpp:230)
//  and the handler it installs ends in abort() (:212), so a std::bad_alloc on
//  this path is not a refused client — it is a panic, provoked by a stranger
//  connecting. The boards that can produce one are the no-PSRAM ones, of which
//  Diag.h has the measurement: a Wireless Stick with its portal on "sits at
//  about 10 KB of byte-addressable RAM and falls to a few hundred bytes under
//  a page request".
//
//  The same hazard on the same kind of task already has its answer one file
//  over: serveGuarded() (src/net/WifiManager.cpp:1353-1358), written because
//  "an allocation failure in one of them used to abort — a browser opening the
//  dashboard rebooted a Wireless Paper three times running" (:1378-1381). The
//  listener never got it.
//
//  So the rule for admitting a client lives here, with no Arduino, no
//  FreeRTOS and no AsyncTCP in it: the caller passes in the six things that
//  touch hardware, and this owns the order they happen in and the accounting
//  when one of them fails. That purity is the point — it is what lets the
//  refusal path be proven on the host (test/test_client_admit) instead of by
//  starving a bench board until a peer connects at the wrong moment.
//
//  The invariant, which is what that proof is about. Stated as outcomes and
//  not as calls, because enrol() *does* run on the failing path — it runs, it
//  throws, and the context comes back here:
//
//    every context allocate() hands back either ends up enrolled, or is
//    released and its client refused. Never both and never neither, and
//    refuse() runs for every client that is not accepted. A refusal is never a
//    leak — not of the client the framework handed over, and not of the
//    context this asked for.
//
//  And two things the rule asks of its caller in return, because it can
//  enforce neither.
//
//  Attach nothing that carries the context — onData, onDisconnect, onError —
//  until Accepted comes back. Attached earlier, a refusal frees a context the
//  framework still holds a pointer to, and the next byte off that socket is a
//  use-after-free instead of a closed connection. Ordering is the whole of the
//  protection.
//
//  And do not keep what allocate() returned. On every outcome but Accepted the
//  context has already gone to release() before the caller sees the answer, so
//  a caller that captured the pointer inside its allocate() callable is holding
//  freed memory — one log line naming ctx->id away from a use-after-free on the
//  event task. Publishing it from the *end* of enrol() instead makes that
//  impossible by construction: enrol() only ever finishes on the path that is
//  about to return Accepted. RetiTransportServer::onClient() does it that way.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Net {
namespace ClientAdmit {

// What became of one connection attempt. Everything but Accepted means the
// client was turned away and freed, and nothing of it is left behind.
enum class Outcome : uint8_t {
  Accepted,            // enrolled; the caller may now attach its callbacks
  RefusedRestarting,   // a restart is pending
  RefusedFull,         // at the client cap
  RefusedNoMemory,     // the context could not be allocated
  RefusedNotEnrolled   // enrolling threw; the context was freed
};

// One admission decision, in the order the costs are incurred.
//
//   cap            the most clients this node holds at once — RNS_MAX_CLIENTS
//                  (Config.h). A parameter rather than an include, so this
//                  header stays clear of the board macros and a test can drive
//                  either arm of it.
//   restarting()   is a restart pending? Asked first, because it is the one
//                  question that costs nothing to ask — no lock, no allocation
//                  — and a client accepted seconds before a restart is
//                  registered with Transport and torn down before it exchanges
//                  a packet. What the caller spends on the *answer* is its own:
//                  RetiTransportServer::onClient() has read the peer's address
//                  before it gets here, because a refusal frees the client, so
//                  the refusal that follows is no longer free the way it was
//                  when it named nobody. Must not throw.
//   count()        clients already enrolled. Asked only when no restart is
//                  pending, and always before anything is allocated. Must not
//                  throw: it is asked outside the try below, so a throw here
//                  reaches the terminate handler — and takes the client with
//                  it, neither refused nor closed, so the socket leaks on the
//                  way down.
//   allocate()     make the per-client context, or return something falsy.
//                  Must not throw: the caller allocates with
//                  `new (std::nothrow)`, which answers a starved heap with a
//                  null pointer instead of the std::bad_alloc that would
//                  reach the terminate handler. That substitution is the
//                  whole exercise.
//   enrol(ctx)     take the lock and put ctx in the list. May throw, and not
//                  as a formality: the push can still have to allocate,
//                  because the reserve in begin() is itself guarded, and a
//                  board too short of memory to satisfy it at boot is left
//                  growing the list on demand exactly as it did before the
//                  reserve existed.
//                  All-or-nothing when it does throw: the list must be left
//                  exactly as it was. The catch below hands the context back to
//                  release(), so a list that had kept it would leave the RNS
//                  task's sendTo() walking a freed pointer — a worse failure
//                  than the panic this replaces. The real enrol() obeys because
//                  std::vector::push_back does: a push that cannot get memory
//                  leaves the vector unchanged, which is push_back's strong
//                  exception guarantee, and is read off a real container in
//                  test_client_admit rather than asserted in prose here. So
//                  anything added to enrol() after that push — the client
//                  count, the publishing of the pointer — must not throw.
//   release(ctx)   free the context. Must not throw.
//   refuse()       turn the client away and free it — close it and let the
//                  disconnect callback delete it, which is what refuse() in
//                  RetiTransportServer.cpp does. Must not throw. And it frees
//                  the client *before it returns* whenever the close succeeds,
//                  because the framework runs that disconnect callback inline;
//                  refuse() in RetiTransportServer.cpp cites the line it does
//                  that on. So the caller's client pointer is spent from the
//                  moment any refusal becomes possible, and anything it wants
//                  to say about the peer — its address above all — has to be
//                  read before admit() is called at all.
//
// No allocation of its own, no clock, no lock: everything that can fail is the
// caller's, and what is here is the order and the accounting.
template <typename Restarting, typename Count, typename Allocate,
          typename Enrol, typename Release, typename Refuse>
inline Outcome admit(size_t cap, Restarting restarting, Count count,
                     Allocate allocate, Enrol enrol,
                     Release release, Refuse refuse) {
  if (restarting()) { refuse(); return Outcome::RefusedRestarting; }

  // The cap before the context, not after it. This is the ordering the reserve
  // in begin() rests on: room is taken once for cap slots, and a peer past the
  // cap is turned away before it can reach a push that would need a cap+1'th.
  if (count() >= cap) { refuse(); return Outcome::RefusedFull; }

  auto ctx = allocate();
  if (!ctx) { refuse(); return Outcome::RefusedNoMemory; }

  try {
    enrol(ctx);
  // Everything, and not only bad_alloc: the handling is identical whatever came
  // out, and a type this did not expect escaping onto a task with nothing above
  // it to catch turns a refused client into the very panic this exists to
  // remove.
  } catch (...) {
    // The list would not take it, and left itself as it was. The context is
    // therefore still this function's to give back — nobody else holds a
    // pointer to it, which is exactly why the caller must neither have attached
    // its callbacks nor kept what allocate() returned — and the client is
    // refused like any other.
    release(ctx);
    refuse();
    return Outcome::RefusedNotEnrolled;
  }
  return Outcome::Accepted;
}

} // namespace ClientAdmit
} // namespace Net
