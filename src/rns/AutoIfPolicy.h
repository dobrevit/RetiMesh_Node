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
//  AutoIfPolicy.h — when a discovery multicast is worth the airtime
//
//  AutoInterface used to multicast its discovery token every 1.6 s around the
//  clock, peers or no peers, stations or no stations. A group-addressed frame
//  goes out at the lowest basic rate — the most airtime-expensive shape there
//  is — and each one drags the Wi-Fi radio out of whatever sleep it had
//  achieved, on a node that may sit alone in a yard for weeks. The rule for
//  when that is worth doing lives here, once, and the task asks.
//
//  The rule: while anyone is around — a station on our access point, our own
//  station association onto a LAN, or a live peer — announce at the RNS
//  cadence, because that is what other AutoInterfaces expect and what keeps
//  peerings alive. When the last of them goes, hold the active cadence for a
//  grace of minutes (a phone whose screen went dark, a client roaming between
//  APs, comes back without ever noticing), then back off to an idle cadence.
//  The moment anyone appears again, answer "send now" — not after whatever
//  remains of a stale idle interval — so a new arrival is peered in seconds.
//
//  Pure — no Arduino, no clock of its own, no Wi-Fi — so the decisions are
//  unit-tested on the host (test/test_autoif_policy) rather than waited out
//  on a bench. Unsigned arithmetic makes every interval hold across a
//  millis() wrap. One caller, one task: not synchronised, like SampleGate.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

class AutoIfPolicy {
public:
  // The active cadence is RNS's own ANNOUNCE_INTERVAL, not ours to tune:
  // peers time discovery out after 22 s, so announcing slower than the
  // reference implementation while someone listens risks their view of us.
  static constexpr uint32_t kActiveMs = 1600;

  // The idle cadence, sent to nobody we know of. The ceiling is the 22 s
  // peering timeout: even a listener we cannot see (a host whose multicast
  // reaches us only one way) must hear from us inside it to keep a peering
  // alive, so 15 s leaves real margin under it while cutting the radio's
  // wake-ups nearly tenfold. Discovery of a new arrival does not ride on
  // this: an arrival that speaks AutoInterface announces itself at 1.6 s,
  // we hear it, and presence snaps us back.
  static constexpr uint32_t kIdleMs = 15000;

  // How long "someone was here" keeps the active cadence after they leave.
  // Minutes: long enough that a screen-off phone or a roaming
  // client returns to a node still announcing at full rate, short enough
  // that a genuinely empty yard stops paying for company within one cup of
  // coffee. Eight times the 22 s peer timeout.
  static constexpr uint32_t kGraceMs = 3u * 60u * 1000u;

  // One decision per pass: whether to send the discovery multicast now.
  // Presence is any of the three ways somebody could hear it — a station
  // associated to our AP, our own STA association, a live peer. The first
  // ask always sends (a task that just started announcing itself is the
  // point), and a rising edge of presence sends immediately.
  bool discoveryDue(uint32_t nowMs, bool anyApStation, bool staConnected, size_t peerCount) {
    const bool present  = anyApStation || staConnected || peerCount > 0;
    const bool first    = !_asked;
    const bool appeared = !first && present && !_wasPresent;
    _asked = true;
    _wasPresent = present;
    // The grace runs from the last moment anyone was present; the first ask
    // seeds it, so a node that boots alone still announces actively for the
    // grace — its own arrival is the event the neighbourhood may care about.
    if (first || present) _lastPresenceMs = nowMs;
    if (nowMs - _lastPresenceMs >= kGraceMs) {
      // Pin the distance at the bound: left to grow, the unsigned difference
      // wraps back under the grace after 49.7 days of absence and the node
      // would spend three minutes announcing actively to nobody, every 49.7
      // days, for no reason it could name.
      _lastPresenceMs = nowMs - kGraceMs;
      _cadenceMs = kIdleMs;
    } else {
      _cadenceMs = kActiveMs;
    }
    if (first || appeared || nowMs - _lastSentMs >= _cadenceMs) {
      _lastSentMs = nowMs;
      return true;
    }
    return false;
  }

  // The cadence the last decision was made at, for the log and the tests.
  uint32_t cadenceMs() const { return _cadenceMs; }

private:
  uint32_t _lastPresenceMs = 0;
  uint32_t _lastSentMs     = 0;
  uint32_t _cadenceMs      = kActiveMs;
  bool     _asked          = false;
  bool     _wasPresent     = false;
};
