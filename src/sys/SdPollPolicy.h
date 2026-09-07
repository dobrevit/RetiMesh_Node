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
//  SdPollPolicy.h — how often the card slot is worth asking about
//
//  Asking costs something, and on an empty slot it costs a lot: the disk
//  layer has nobody to talk to and spends about half a second waiting for an
//  answer before it gives up (SdCard::probe()). The poll task paid that every
//  three seconds — a sixth of that task's waking life, and a sixth of the
//  card's SPI bus, spent proving over and over that the slot nobody put a
//  card in is still empty. On the T-Deck, where the card shares its bus with
//  the panel and the radio, it is not even its own bus to burn.
//
//  So the cadence follows the answer. Nothing there, and nothing there again,
//  means the next answer is very likely the same one: the interval doubles
//  each time until it reaches a ceiling, and anything that turns up in the
//  slot drops it straight back to where it started. A mounted card is a
//  different question — its periodic raw read is a removal check, not a
//  search — and it gets an interval of its own, long enough that the card can
//  settle into the idle state its own controller offers between touches.
//
//  Two ladders, not one
//  --------------------
//  A card that is in the slot but will not mount — blank, ext4, exFAT, or one
//  whose mount failed — is a card somebody is about to format, so it is
//  looked at on the base beat. But "about to" has a length, and it is minutes
//  at most: `unformatted` is also a steady state an operator can leave a node
//  in for months (docs/troubleshooting.md), and holding the base beat for it
//  for ever would spend the whole saving this file exists for on the one card
//  nobody is coming back to. So that state gets a ladder of its own: the base
//  beat for long enough to cover somebody standing at the node
//  (kUnmountableHoldLooks, about a minute and a half), then the same doubling
//  up to the same ceiling. Nothing is lost by climbing — the caller's wait is
//  cut short the moment a format is asked for, which is the thing the base
//  beat was really protecting.
//
//  What the ceiling costs
//  ----------------------
//  The ceiling is the operator's worst case for "I have put a card in; when
//  does the node notice". Half a minute, because:
//
//   * it takes the probe's share of the wall clock from about 17 % (half a
//     second in every three) to about 1.7 %. That is the win, and it is
//     nearly all of the win that is there to have;
//   * a further doubling to a minute would take it to 0.85 % — under a
//     percentage point of one peripheral's time — while doubling the wait
//     the operator is actually standing there for. The trade stops paying;
//   * and it matches the mounted card's keep-alive, so a settled node looks
//     at the slot on one half-minute beat whatever is in it, which is one
//     less thing to hold in your head when reading a log.
//
//  Inserting a card is a maintenance action, not a data path: nothing routes
//  or stores through the slot until it has been mounted, and where the
//  Reticulum store lives is decided at boot and never at runtime. Half a
//  minute of patience is the whole price.
//
//  What resets it
//  --------------
//  The ladder has no notion of anybody being present, and must not be given
//  one by accident: a reset on every /api/status read would put the node back
//  on the three-second beat for as long as any monitoring tool kept polling,
//  which is for ever. Two things put it back on the base beat, and nothing
//  else does:
//
//   * the slot's own answer changing — a card turns up, or a mounted one is
//     lost. Then the next look is three seconds away, because that is the
//     moment somebody is most likely to be doing something about it;
//   * wake(), raised by an unambiguous operator action. Today that is the
//     button on the node (SdCard::lookNow(), called where the same press
//     wakes an idled-down access point): a hand on the node is the one
//     signal that means somebody is there, and it is the same hand that
//     pushes a card in.
//
//  So a node that has been up an hour with an empty slot notices a card
//  within half a minute, or at once if whoever inserted it presses the
//  button.
//
//  Pure — no Arduino, no FreeRTOS, no clock of its own, and no timestamps at
//  all: the caller is handed an interval and waits it out, so there is
//  nothing here for a millis() wrap to reach. The unbounded quantities are
//  the two look counts instead, and they saturate. Unit-tested on the host
//  (test/test_sd_poll_policy) rather than waited out on a bench. One caller,
//  one task: not synchronised, like SampleGate and AutoIfPolicy.
// ============================================================================
#pragma once

#include <stdint.h>

class SdPollPolicy {
public:
  // The interval the slot has always been checked at. It is still the answer
  // for the first empty look, and for the first minute and a half of anything
  // sitting in the slot unmounted: a node that has just lost a card, or has
  // one in it that wants formatting, is a node somebody is standing in front
  // of — for a while.
  static constexpr uint32_t kBaseMs = 3000;

  // Where the doubling stops — see what the ceiling costs, above.
  static constexpr uint32_t kAbsentCeilingMs = 30000;

  // The mounted card's keep-alive: the raw read of sector zero that notices a
  // removal, and the only thing this firmware does to a card nobody is using.
  // (A card that is being used is touched by whoever is using it: SdCard::log()
  // appends from the RNS and OTA tasks, and where the Reticulum store lives on
  // the card, microStore writes to it continuously.) The same half minute as
  // the ceiling, but bounded by a different question — how late the node may
  // notice a card has gone — and the answer is that nothing depends on
  // noticing promptly: the log append fails on its own, and a store that was
  // on the card is frozen from the moment it left, whether the poll has caught
  // up or not. What that lateness does reach is every surface that reports the
  // card; docs/api.md says so.
  static constexpr uint32_t kPresentMs = 30000;

  // How many consecutive looks at a card that will not mount are served at the
  // base beat before the same doubling applies to it. Thirty of them is a
  // minute and a half — long enough to cover somebody who has just pushed a
  // blank card in and is walking round to the portal, short enough that a node
  // left with one in it for a season is not still probing every three seconds
  // in the spring.
  static constexpr uint32_t kUnmountableHoldLooks = 30;

  // Doublings are clamped before the shift, so a slot left empty for months
  // cannot walk the shift off the end of a uint32_t. The ceiling is reached
  // at the fourth doubling; this bound only exists to make the shift provably
  // safe.
  static constexpr uint32_t kMaxDoublings = 16;

  // Where the look counters stop, and the reason they stop one above
  // kMaxDoublings: rung n doubles n-1 times, so kMaxDoublings + 1 is the first
  // count that already produces the largest shift the clamp allows. Counting
  // past it changes no answer and only risks a wrap.
  static constexpr uint32_t kMaxRung = kMaxDoublings + 1;

  // The piece a long wait is served in, so the caller can feed the task
  // watchdog between the pieces. The base beat, because that is the cadence
  // the poll task reported progress on before any of this existed, and the
  // supervision should not have noticed this change at all.
  static constexpr uint32_t kSliceMs = kBaseMs;

  // Where the unmountable counter stops: the hold, plus exactly enough rungs
  // to land on the same last rung the empty-slot ladder stops at. Counting
  // past it changes no answer.
  static constexpr uint32_t kMaxUnmountableLooks = kUnmountableHoldLooks + kMaxRung - 1;

  // The wait before the next look, decided by what this one found. `mounted`
  // wins over `cardInSlot`, which it implies.
  uint32_t nextWaitMs(bool mounted, bool cardInSlot) {
    if (mounted)    { _misses = 0; _stuck = 0; return kPresentMs; }
    if (cardInSlot) {
      _misses = 0;
      if (_stuck < kMaxUnmountableLooks) _stuck++;              // saturates
      return unmountableMs(_stuck);
    }
    _stuck = 0;
    if (_misses < kMaxRung) _misses++;             // saturates; the ladder is
    return absentMs(_misses);                      // flat long before
  }

  // An operator asked for a look. Both ladders go back to their first rung, so
  // this look and the next few after it are on the base beat again. Modelled
  // on ApIdlePolicy::wake(): a policy with no notion of who is present is told
  // by the one event that proves somebody is — and by nothing else, or the
  // back-off is defeated by whatever polls the node most often.
  void wake() { _misses = 0; _stuck = 0; }

  // The empty-slot ladder as a value: the interval after `misses` consecutive
  // empty looks, where the first miss waits the base and each one after that
  // doubles. The domain starts at 1 — zero misses is not a rung, and the base
  // it answers for 0 is only what makes the arithmetic below total. Static, so
  // the whole curve can be read off in a test without stepping a policy
  // through it, and so the growth is written down once.
  static constexpr uint32_t absentMs(uint32_t misses) {
    const uint32_t steps = misses < 2 ? 0u
                         : (misses - 1 > kMaxDoublings ? kMaxDoublings : misses - 1);
    const uint32_t ms = kBaseMs << steps;
    return ms > kAbsentCeilingMs ? kAbsentCeilingMs : ms;
  }

  // The unmountable-card ladder, likewise from 1: flat at the base for the
  // hold, then the empty-slot ladder from its second rung on, so the base beat
  // is served once — for the whole hold — and never again.
  static constexpr uint32_t unmountableMs(uint32_t looks) {
    return looks <= kUnmountableHoldLooks ? kBaseMs
                                          : absentMs(looks - kUnmountableHoldLooks + 1);
  }

  // How a wait is served in watchdog-sized pieces, as arithmetic rather than
  // as a loop condition at the call site: the next piece of what is left, and
  // how many pieces the whole interval becomes. Every interval this policy can
  // answer today is a whole number of slices, which makes the short final
  // piece unreachable and therefore untestable at the caller — so the rule is
  // pinned here instead, and a retune that stops dividing evenly still gets
  // its remainder waited out.
  static constexpr uint32_t nextSliceMs(uint32_t leftMs) {
    return leftMs < kSliceMs ? leftMs : kSliceMs;
  }
  static constexpr uint32_t slicesFor(uint32_t ms) {
    return ms / kSliceMs + (ms % kSliceMs ? 1u : 0u);
  }

  // Consecutive looks of each kind, for the tests and for anyone reading a log.
  uint32_t misses() const { return _misses; }
  uint32_t unmountableLooks() const { return _stuck; }

private:
  static_assert(kAbsentCeilingMs >= kBaseMs, "the ceiling cannot sit below the base");
  static_assert(kBaseMs <= (UINT32_MAX >> kMaxDoublings), "the doubling must not overflow");
  static_assert(kUnmountableHoldLooks >= 1, "the unmountable state gets at least one base look");

  uint32_t _misses = 0;      // consecutive empty looks
  uint32_t _stuck  = 0;      // consecutive looks at a card that will not mount
};
