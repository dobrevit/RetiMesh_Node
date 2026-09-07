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
//  GnssDutyPolicy.h — when a receiver is allowed to stop looking
//
//  A GNSS receiver is the most expensive part on most of these boards that
//  nobody is using: tens of milliamps, continuously, for a number that on a
//  node bolted to a mast has not changed since it was bolted there. Until now
//  it ran flat out whenever it was powered, and the only control an operator
//  had was to switch it off entirely and lose the clock with it.
//
//  This is the rule for the middle ground, and it is three sentences:
//
//    * a receiver that has not found itself is never rested. You cannot duty-
//      cycle a search. Stopping one halfway does not save the energy, it
//      spends it again from the start — and on a node that has just been
//      powered up in a new place, which is precisely when somebody is waiting
//      for the answer;
//    * a screen whose purpose is to show where the node is holds the receiver
//      awake for as long as it is being looked at. Somebody watching a
//      position wants it to move when they do, and a stale reading under a
//      live-looking dial is worse than a slow one;
//    * otherwise a node that knows where it is may stop looking, for as long
//      as its role says its answer stays good for.
//
//  The role, and why the rule needs one
//  ------------------------------------
//  How long an answer stays good is not a property of the hardware, so it
//  cannot be inferred from the board: the same firmware on the same board is a
//  handheld in somebody's pocket on one desk and a relay screwed to a mast on
//  the next. It is the operator's statement about what the node is for, which
//  is why it is a setting (transport.node_role) and not a #define.
//
//  Nothing is inferred from an unrecognised value either. A node that has
//  never been told what it is tracks continuously, exactly as every node did
//  before this file existed — which is what makes a firmware upgrade a
//  no-change for the fleet, and what makes a role added by a later firmware
//  and then downgraded away fail safe rather than fail quiet.
//
//  That last clause is about *reading*, and only about reading. Writing is
//  narrower: SettingsRules rejects a stored role above the highest this build
//  knows, exactly as it does for the power profile — so a node downgraded onto
//  a firmware that has never heard of its role goes on behaving safely, but
//  refuses every transport settings change until the role is rewritten to one
//  this build recognises. That is the deliberate trade (an unknown number must
//  not become a *different* role the day a later firmware defines it), and it
//  is worth knowing before reading "safe to read here" as "safe".
//
//  The cadences, and where each number comes from
//  ---------------------------------------------
//  kSettleMs — how long a fix has to stand before it counts as found. Every
//  receiver here reports at 1 Hz, so five seconds is five independent
//  assertions rather than one sentence that happened to arrive during a
//  momentary lock; it is also long enough for a date-bearing RMC to have had
//  its chance at the clock, which is the other thing the receiver is for.
//
//  kRestTransportMs — five minutes. A node that does not move re-earns the
//  same coordinates for ever; what it does still want is a clock that has not
//  drifted and a chance to notice it has been carried off. Five minutes bounds
//  both, and it is short enough that every wake is a hot start: the receiver
//  keeps its ephemeris for hours, so nothing is re-downloaded.
//
//  kRestCarriedMs — one minute. A carried node moves, and its position is read
//  when the glass comes back on, when a message is stamped, and when a bearing
//  is drawn. At walking pace a minute is under a hundred metres of error in
//  the worst case, which is inside what any of those three uses can tell.
//
//  There is deliberately no cap on the tracking window. The obvious pairing —
//  "rest five minutes, track thirty seconds" — spends a fixed thirty seconds
//  whether the receiver locked in two or has not locked at all, and the second
//  half of that is the case the first rule already covers better: with no fix
//  there is no rest, so a receiver that cannot see the sky simply keeps
//  looking, and one that locks immediately pays only kSettleMs.
//
//  What a rest does to the rest of the driver
//  ------------------------------------------
//  Four more decisions hang off this schedule, and every one of them is
//  arithmetic over scalars rather than anything to do with a port: whether a
//  fix that has stopped being re-asserted is stale or merely quiet because we
//  told it to be; whether a fix counts towards the *next* rest; whether
//  anybody is actually looking at a position; and whether a receiver that
//  ought to be talking should be prodded again. They are stated here, beside
//  the schedule they belong to, so they are pinned on the host with it rather
//  than only ever executed on a bench beside one board.
//
//  Pure — no Arduino, no driver, no port, no clock of its own: the caller
//  passes the time in. So the schedule is pinned on the host
//  (test/test_gnss_duty) rather than by sitting next to a receiver with a
//  current probe. Which command or which pin carries a verdict to which board
//  is not decided here; that is Gps.cpp, and it differs per board because the
//  hardware does.
// ============================================================================
#pragma once

#include <stdint.h>

class GnssDutyPolicy {
public:
  // What the receiver should be doing. Rest means "it may stop"; whether that
  // is a pin, a rail or a message is the driver's business.
  enum class Verdict : uint8_t { Track, Rest };

  // Power::Role's ordinals, which are what the setting stores. Written out
  // rather than included because this header is host code and Power.h is
  // Arduino code; Power.cpp holds the static_asserts that keep the two in
  // step, so a renumbering there is a build error rather than a silent change
  // of what a stored 1 means.
  static constexpr uint8_t kRoleUnset     = 0;
  static constexpr uint8_t kRoleCarried   = 1;
  static constexpr uint8_t kRoleTransport = 2;

  // The cadences, argued for in the comment above.
  static constexpr uint32_t kSettleMs         = 5000;
  static constexpr uint32_t kRestCarriedMs    = 60000;
  static constexpr uint32_t kRestTransportMs  = 300000;

  // A fix is reported stale rather than wrong once the receiver stops
  // re-asserting it. Every receiver here reports at 1 Hz, so ten seconds is
  // ten missed assertions rather than one late one.
  static constexpr uint32_t kFixTimeoutMs     = 10000;
  // How long one paint of a position screen holds the receiver awake. Longer
  // than the slowest of those screens' own refresh timers — the sky view runs
  // at two seconds — so a page still on the glass is never mistaken for one
  // that has gone; short enough that a page that *has* gone releases the
  // receiver inside one settle window, so closing it costs no rest.
  static constexpr uint32_t kNavClaimMs       = 5000;
  // How long a receiver that ought to be talking may stay silent before it is
  // prodded again. One receiver-second of margin over the 1 Hz every part here
  // reports at, four times over.
  static constexpr uint32_t kNudgeMs          = 5000;

  // What the node's state is, whole, on every ask. The caller passes all of it
  // rather than the edge it noticed, so no input can be left holding a value
  // from a state that has since moved.
  struct State {
    uint8_t role   = kRoleUnset;   // the operator's statement about this node
    bool    fix    = false;        // the receiver is asserting a fix *now*
    bool    navLit = false;        // a screen that shows position is being painted
  };

  // Whether this role has a rest at all. Every value this firmware does not
  // recognise answers no, which is the safe direction: an unknown role is a
  // node whose behaviour nobody has decided, and the behaviour nobody decided
  // is the one it already had.
  static constexpr bool dutyCycles(uint8_t role) {
    return role == kRoleCarried || role == kRoleTransport;
  }

  // How long the rest lasts for a role. Zero for a role that does not rest,
  // which no caller reaches — dutyCycles() has already said no.
  static constexpr uint32_t restMsFor(uint8_t role) {
    return role == kRoleTransport ? kRestTransportMs
         : role == kRoleCarried   ? kRestCarriedMs
         :                          0;
  }

  // --- what the rest does to everything around it --------------------------
  //
  // Every one of these is a function of the caller's scalars and nothing else,
  // and every comparison in them is on a difference of two stamps rather than
  // on their order, so the 49.7-day millis() wrap costs them nothing either.

  // Whether the receiver's silence is this node's own doing. One that has been
  // told to stop talking, or one that has only just been told it may start
  // again, is not one that has lost its fix. `trackFromMs` is when the current
  // tracking window began — the wake, or the switch-on.
  static constexpr bool ourSilence(bool resting, uint32_t nowMs, uint32_t trackFromMs) {
    return resting || nowMs - trackFromMs < kFixTimeoutMs;
  }

  // Whether a fix still flagged valid should now be dropped as stale. The
  // suppression above is the whole of the operator guarantee that a resting
  // node keeps reporting its last position instead of pretending it lost one:
  // without it every rest longer than the timeout would report "no fix" for
  // its whole length, which is both wrong and the opposite of what resting is
  // for. The wake gets the same grace a lost signal already gets, so a
  // receiver has a full timeout to say the first thing it says.
  static constexpr bool dropStaleFix(bool resting, bool fixValid, uint32_t nowMs,
                                     uint32_t lastFixMs, uint32_t trackFromMs) {
    return fixValid && !ourSilence(resting, nowMs, trackFromMs) &&
           nowMs - lastFixMs >= kFixTimeoutMs;
  }

  // Whether the receiver has found itself *again* since it was allowed to
  // look. This, and not a valid flag on its own, is what update()'s `fix`
  // should be fed: after a rest that flag still carries the position held
  // before it, and a fix stamped before the rest cannot answer the question.
  // Without it a receiver that never re-acquires would rest for ever on the
  // strength of a fix it held an hour ago.
  static constexpr bool fixReacquired(bool fixValid, uint32_t lastFixMs,
                                      uint32_t trackFromMs) {
    return fixValid && (int32_t)(lastFixMs - trackFromMs) >= 0;
  }

  // "Somebody is looking at where this node is": a position screen painted
  // recently, and the glass actually lit. A claim that expires rather than a
  // flag somebody has to clear — those screens are torn down four different
  // ways and a flag leaked by any one of them would hold the receiver awake
  // for ever. `navPaintedMs` zero is the caller's "never claimed".
  static constexpr bool navLit(bool screenDark, uint32_t nowMs, uint32_t navPaintedMs) {
    return !screenDark && navPaintedMs && nowMs - navPaintedMs < kNavClaimMs;
  }

  // Since when a receiver has had nothing to say: its last sentence, or the
  // instant it was allowed to look, whichever is later — and the latter alone
  // before it has ever said anything (`lastSentenceMs` zero). Later of the
  // two, because a rest is silence by arrangement: measuring from the last
  // sentence alone would have the first pass after a wake count the whole rest
  // as a fault and prod a receiver that has not had a character-time to
  // answer in.
  static constexpr uint32_t quietSince(uint32_t lastSentenceMs, uint32_t trackFromMs) {
    if (!lastSentenceMs) return trackFromMs;
    return (int32_t)(lastSentenceMs - trackFromMs) >= 0 ? lastSentenceMs : trackFromMs;
  }

  // Whether a receiver that ought to be talking should be prodded again. On
  // the one board where the ask is a message rather than a pin there is no
  // rail to power-cycle a module that failed to hear its wake, so a receiver
  // that has gone quiet past the interval is nudged — but only where this run
  // has actually asked one to stop (`everAsked`: a power-management request
  // has gone out since switch-on). Not asked, nothing to recover from: a node
  // whose role does not duty-cycle, or a board with an empty receiver socket,
  // must not transmit anything it would not have transmitted before this file
  // existed. Never while resting, which is the silence working.
  static constexpr bool nudgeDue(bool resting, bool everAsked, uint32_t nowMs,
                                 uint32_t quietSinceMs, uint32_t lastNudgeMs) {
    if (resting || !everAsked) return false;
    return nowMs - quietSinceMs >= kNudgeMs && nowMs - lastNudgeMs >= kNudgeMs;
  }

  // One pass. Call it as often as is convenient — the answer is a function of
  // the state and the clock, not of how often it was asked — and act on it
  // when it differs from the last one.
  Verdict update(uint32_t nowMs, const State& s) {
    // The two ways a rest is refused outright. Both are re-checked every pass,
    // which is what lets a nav screen opened mid-rest end it at once rather
    // than at the end of it.
    if (!dutyCycles(s.role) || s.navLit) {
      wake();
      return Verdict::Track;
    }
    if (_resting) {
      // The fix is deliberately not consulted here. Nothing is arriving from a
      // receiver that has been asked to stop, so "no fix" during a rest is the
      // rest working rather than the receiver failing — and reading it would
      // cancel every rest a second after it started, which is exactly the loop
      // the first rule would otherwise create with itself.
      if (nowMs - _restSince < restMsFor(s.role)) return Verdict::Rest;
      wake();
      return Verdict::Track;
    }
    // Tracking. Nothing may rest until the receiver has actually found itself
    // and stood by it.
    if (!s.fix) { _fixSeen = false; return Verdict::Track; }
    if (!_fixSeen) { _fixSeen = true; _fixSince = nowMs; return Verdict::Track; }
    if (nowMs - _fixSince < kSettleMs) return Verdict::Track;
    _resting = true;
    _restSince = nowMs;
    _fixSeen = false;
    return Verdict::Rest;
  }

  // What the last pass decided, for the driver's edge test and for reading a
  // log beside a receiver that did not go quiet.
  bool resting() const { return _resting; }
  // Whether a fix is standing but has not stood long enough yet. For the
  // tests, and for the same log.
  bool settling() const { return _fixSeen; }

private:
  // Every comparison here is on a difference of two stamps, never on their
  // order, so the 49.7-day millis() wrap costs nothing — and "no stamp yet" is
  // a flag rather than a reserved value, so the one millisecond per boot when
  // millis() reads zero is an ordinary instant like any other.
  void wake() {
    _resting = false;
    // The settle window starts again from the wake, not from whatever the fix
    // was doing before the rest: the question after a rest is whether the
    // receiver has found itself *again*, and a stamp from before it was asked
    // to stop cannot answer that.
    _fixSeen = false;
  }

  // Tracking at construction, which is the state a receiver is switched on in.
  bool     _resting  = false;
  uint32_t _restSince = 0;
  bool     _fixSeen   = false;
  uint32_t _fixSince  = 0;
};
