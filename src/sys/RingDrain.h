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
//  RingDrain.h — empty a ring in bounded bites, and keep reporting while you do
//
//  The other half of RingItem. That one makes sure a borrowed item is always
//  given back; this one makes sure the loop borrowing them always ends.
//
//  The inbound TCP ring is filled on core 0 by AsyncTCP at whatever rate a
//  host on the access point cares to push, and was drained on the RNS task by
//  a loop that ran until the ring was empty. Those two rates have nothing to
//  do with each other: a peer on the LAN can refill the ring as fast as the
//  drain empties it, and the drain then does not return. Ring residency is not
//  the bound people reach for first — the loop ends when the consumer catches
//  up, not when the ring's capacity is reached, so a ceiling on the standing
//  backlog says nothing about how many times round the loop goes.
//
//  Nothing else in the pass runs while it does not return, and that includes
//  the reporting. The RNS task feeds the watchdog once, at the top of each
//  pass (main.cpp), and before this helper the next feeds in the pass were
//  refreshSnapshots()'s three — which run after reticulum.loop(), in a
//  Diag::guard of their own. reticulum.loop() is library code and feeds
//  nothing of its own. So between the feed at the top of the pass and the walk
//  at the end of it there was no reporting at all, and a flood from a Wi-Fi
//  client can hold the Reticulum task past its thirty-second watchdog: a
//  task-watchdog reset naming "rns".
//
//  That last step is read off the code, not off a log. No such reset has been
//  observed on this ring: the two-day reboot loop the neighbouring scar
//  describes is the path-table sweep's (RnsTransport.cpp, kWalkBudgetMs), a
//  different unbounded loop, and the bench reproduction for this one —
//  flooding the ring from a host on the access point — has not been run.
//  Where the reset itself is named it is named as something that *can*
//  happen — docs/api.md, drainTcp() in RnsTransport.cpp and
//  test/test_ring_drain all say "can" — and the counter's other surfaces
//  describe what the cap does rather than a reset. None of them says one was
//  seen.
//
//  A pass therefore takes a bite, feeds the watchdog while it chews, and comes
//  back for the rest on the next tick. Nothing here knows what a ring is: the
//  receive, the handler, the feed and the "is there more" are all the
//  caller's, which is what lets the shape be proven on the host
//  (test/test_ring_drain) rather than by flooding a bench board and watching
//  it not reboot. The radio ring is drained through it too, for the feed as
//  much as for the bound: that loop sits inside reticulum.loop() and so had no
//  feed of any kind.
// ============================================================================
#pragma once

#include <stddef.h>

namespace Sys {
namespace RingDrain {

// Items one pass may take off a ring.
//
// 64 is where AutoInterface's kDrainBatch landed against the same hazard — a
// producer that can outrun its consumer for as long as it likes — and is taken
// from there as the precedent for the value, not as a coupling. The two are
// independent tunables that happen to agree: that one bounds datagrams per
// select pass on the core-0 AutoInterface task, this one bounds ring items per
// RNS pass on core 1, and either can be retuned without the other.
//
// Nothing is dropped by hitting it: the remainder stays in the ring and the
// caller is back for it on its next tick. What it bounds is one pass, not a
// rate — without it the count per pass had no bound at all. A pass may take
// 64, so on the RNS task's 10 ms loop it permits *up to* 6400 items a second,
// and even that is a ceiling rather than a promise: a pass is the 10 ms delay
// plus the pass itself, and refreshSnapshots() may spend up to kWalkBudgetMs
// — 400 ms — on its record walk, so the sustained rate can be well below it.
// High enough not to bind on ordinary traffic, few enough that no single pass
// runs long — that gap is what this is meant to sit in, and deferring a
// flood's remainder by a tick is what it is for.
constexpr size_t kBatch = 64;

// Items between watchdog feeds, and the one definition of that cadence: the
// record walk in refreshSnapshots() reads it from here rather than keeping its
// own 16, so the two cannot drift apart. The cap ends the pass, but it is only
// checked between items and one item can be a whole packet through
// microReticulum — so the drain keeps reporting while it runs rather than
// relying on finishing.
constexpr size_t kFeedEvery = 16;

// What one pass did.
struct Pass {
  size_t handled = 0;   // items taken off the ring and handed to the caller

  // The cap ended the pass *and* the ring still had something in it — not
  // "the pass happened to take exactly its allowance and the ring is now
  // empty". From inside the loop those two are the same event; to anyone
  // counting how often this node is being outrun they are opposites, so the
  // ring is asked rather than guessed at. (A producer that posts between the
  // last receive and the asking also reads as capped, which is the honest
  // answer: that item is waiting for the next tick either way.)
  bool capped = false;
};

// One bounded pass over a ring.
//
//   receive()  take the next item, or return something falsy when there is
//              none. Whatever it returns goes straight to handle(); this never
//              looks inside it, copies it, or keeps it.
//   handle(i)  do the work and give the item back (Sys::RingItem). May throw:
//              an item counted here has already left the ring whether or not
//              handle() returned, which is what keeps the count honest under a
//              bad_alloc out of the library.
//   feed()     say this task is still moving. Must not throw. It is called
//              with an item already out of the ring and nothing holding it —
//              the Sys::RingItem that gives it back is constructed inside
//              handle(), which has not run yet — so a throw here would leak
//              that item's ring space. Watchdog::feed() cannot throw today;
//              it is stated because RingItem.h exists precisely so the return
//              is structural rather than remembered, and a feed that grew a
//              throw would put this one item back outside that guarantee.
//   anyLeft()  is anything still in the ring? Asked at most once a pass, and
//              only when the cap is what ended it. A caller with nothing to
//              report may return false; Pass::capped is then simply never set.
//
// No allocation, no clock, no ring of its own: this runs on the RNS task,
// which must not block and must not assume a heap.
template <typename Receive, typename Handle, typename Feed, typename AnyLeft>
inline Pass drain(size_t cap, size_t feedEvery,
                  Receive receive, Handle handle, Feed feed, AnyLeft anyLeft) {
  Pass pass;
  // A zero cap takes nothing, so there is nothing it could have deferred and
  // the ring must not be asked: anyLeft() would report a pass that did no work
  // as capped. Neither caller can pass one — both use kBatch — but neither can
  // pass a zero cadence either, and that case is defended a line below.
  if (!cap) return pass;
  // A zero cadence is a modulo by zero — a crash, not a misconfiguration.
  const size_t every = feedEvery ? feedEvery : 1;
  bool ranDry = false;
  while (pass.handled < cap) {
    auto item = receive();
    if (!item) { ranDry = true; break; }
    // Feeds bracket the work the way the snapshot walk's do: one before the
    // first item, one every `every` items through it, one after the last. Not
    // before the receive, though — this is called a hundred times a second on
    // a ring that is usually empty, and a pass with no work has nothing to
    // report and no reason to take the watchdog's lock to say so.
    if (pass.handled == 0) feed();
    // Counted, and fed for, before the item is handled: as in the walk, the
    // feed has to come before the expensive part, not after it.
    if (++pass.handled % every == 0) feed();
    handle(item);
  }
  if (pass.handled) feed();
  // Only the cap can defer anything. A ring that ran dry deferred nothing, and
  // asking it would race a producer into a "capped" that never happened.
  if (!ranDry) pass.capped = anyLeft();
  return pass;
}

} // namespace RingDrain
} // namespace Sys
