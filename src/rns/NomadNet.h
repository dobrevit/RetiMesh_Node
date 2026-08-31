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
//  NomadNet.h — the node's own page, in micron
//
//  A RetiMesh node already has a status page: it is served over HTTP, on the
//  access point, to somebody standing next to it. The node that most needs to
//  be asked how it is doing is the one on a hill that nobody is standing next
//  to, and for that node the portal is the wrong answer — it needs Wi-Fi
//  association, an IP, a browser and line of sight to the antenna.
//
//  NomadNet browses over Reticulum instead. A node with a page answers over
//  whatever carried the request: the LoRa channel, a TCP peer, AutoInterface
//  on the LAN. Same node, same figures, reachable from the other side of the
//  valley by an application people already run, with nothing to install and
//  no web server on the node at all.
//
//  What it serves is generated rather than authored. An operator with a
//  bulletin board to publish wants files on storage and that is a different
//  feature; what every node has, on every board, with nothing uploaded, is an
//  honest account of itself — which channel, which peers, how much heap, how
//  many messages and how many of them it could actually check.
//
//  The markup is micron, NomadNet's own: `>` heads a section, a backtick
//  begins a formatting escape (`!` bold, `` ` `` resets), a lone `-` draws a
//  divider. A client that does not understand micron shows the text, which is
//  most of the point of the format.
//
//  Pure, and rendered into a caller's buffer, so what a stranger can ask this
//  node to produce is bounded and can be read on a host.
// ============================================================================
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace Rns {
namespace NomadNet {

// What a node can say about itself. Gathered by the caller, so this file
// touches no hardware — and, as with telemetry, a figure the board cannot
// take is marked absent rather than sent as a zero, because a zero is a
// reading and an absence is not.
struct Status {
  const char* name = "";
  const char* version = "";
  const char* board = "";
  const char* address = "";        // this node's nomadnetwork.node hash, hex
  const char* lxmfAddress = "";    // where a person can message it
  uint32_t    uptimeS = 0;

  bool        radioOnline = false;
  const char* radioModel = "";
  float       freqMhz = 0, bwKhz = 0;
  uint8_t     sf = 0, cr = 0;
  int8_t      txDbm = 0;
  bool        heardAnything = false;
  float       lastRssi = 0, lastSnr = 0;
  uint32_t    rxPackets = 0, txPackets = 0;

  uint32_t    interfaces = 0, paths = 0, links = 0;

  uint32_t    lxmfRx = 0, lxmfUnverified = 0, lxmfMismatched = 0;

  bool        haveBattery = false;
  uint8_t     batteryPct = 0;
  bool        charging = false, chargeKnown = false;
  uint32_t    heapFree = 0, heapLargest = 0;
};

// A bounded appender. Once a write does not fit the page is marked short and
// every later write is a no-op, so a caller checks once at the end — and a
// page that ran out says so in its last line rather than stopping mid-figure
// and looking like the node died halfway through answering.
class Page {
public:
  Page(char* out, size_t cap) : _p(out), _cap(cap) { if (cap) _p[0] = '\0'; }
  size_t size() const { return _n; }
  bool   ok() const { return _ok; }

  Page& line(const char* fmt, ...) {
    if (!_ok || _n + 1 >= _cap) { _ok = false; return *this; }
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(_p + _n, _cap - _n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= _cap - _n) { _p[_n] = '\0'; _ok = false; return *this; }
    _n += (size_t)k;
    return *this;
  }

private:
  char*  _p;
  size_t _cap;
  size_t _n = 0;
  bool   _ok = true;
};

// The node's page. Returns bytes written, and always leaves a readable page:
// if the buffer runs out the text ends on a whole line and says so, because a
// page truncated mid-figure reads as a node that failed halfway through
// answering rather than one whose page was too long.
inline size_t index(const Status& s, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  // Room for the notice is taken out of the page's budget before a word is
  // written, rather than looked for afterwards. Looked for afterwards there
  // was none — the page had already used every byte — so a cut page said
  // nothing about being cut. And measuring the notice by writing it left
  // snprintf's partial output in the buffer past the length being returned,
  // which is the same trap the signal report fell into: snprintf writes what
  // it can before telling you it did not fit.
  static const char kCut[] = "\n`!(this page was cut short)``\n";
  const size_t cutLen = sizeof(kCut) - 1;
  if (cap < cutLen + 64) { out[0] = '\0'; return 0; }
  Page p(out, cap - cutLen);

  p.line(">%s\n\n", s.name && s.name[0] ? s.name : "RetiMesh Node");
  p.line("`!%s`` on `!%s``\n", s.version, s.board);
  p.line("Up %lu d %lu h %lu m\n\n",
         (unsigned long)(s.uptimeS / 86400),
         (unsigned long)((s.uptimeS % 86400) / 3600),
         (unsigned long)((s.uptimeS % 3600) / 60));

  p.line(">>Reach me\n");
  p.line("Message this node over LXMF at\n`!%s``\n\n", s.lxmfAddress);
  p.line("This page is at\n`!%s``\n\n", s.address);

  p.line(">>Radio\n");
  if (!s.radioOnline) {
    p.line("The transceiver is `!offline``.\n\n");
  } else {
    p.line("%s at %.3f MHz, %.0f kHz, SF%u, CR 4/%u, %d dBm\n",
           s.radioModel, (double)s.freqMhz, (double)s.bwKhz,
           (unsigned)s.sf, (unsigned)s.cr, (int)s.txDbm);
    // Only where something has actually been heard. A node that has heard
    // nobody reports no signal rather than 0 dBm, which reads as a very loud
    // neighbour.
    if (s.heardAnything)
      p.line("Last heard at %.0f dBm, %.1f dB SNR\n", (double)s.lastRssi, (double)s.lastSnr);
    else
      p.line("Nothing heard yet on this channel\n");
    p.line("%lu received, %lu sent\n\n",
           (unsigned long)s.rxPackets, (unsigned long)s.txPackets);
  }

  p.line(">>Transport\n");
  p.line("%lu interface%s, %lu path%s known, %lu link%s open\n\n",
         (unsigned long)s.interfaces, s.interfaces == 1 ? "" : "s",
         (unsigned long)s.paths, s.paths == 1 ? "" : "s",
         (unsigned long)s.links, s.links == 1 ? "" : "s");

  p.line(">>Messages\n");
  p.line("%lu received\n", (unsigned long)s.lxmfRx);
  // The three standings kept apart, as everywhere else: a sender this node
  // never heard announce is a different thing from one whose signature did
  // not match, and collapsing them is what hid a real defect for a release.
  p.line("%lu from a sender it could not check, %lu that did not match\n\n",
         (unsigned long)s.lxmfUnverified, (unsigned long)s.lxmfMismatched);

  p.line(">>Health\n");
  if (s.haveBattery) {
    if (s.chargeKnown)
      p.line("Battery %u%%, %s\n", (unsigned)s.batteryPct, s.charging ? "charging" : "discharging");
    else
      // The board can measure the cell and cannot see the charger. Saying
      // "discharging" would send somebody looking for a fault in a working
      // cable (Power.h).
      p.line("Battery %u%%\n", (unsigned)s.batteryPct);
  }
  p.line("%lu KB heap free, largest block %lu KB\n",
         (unsigned long)(s.heapFree / 1024), (unsigned long)(s.heapLargest / 1024));

  if (!p.ok()) {
    // Whatever fits, ending on a whole line, and honest about being cut. The
    // room was reserved above, so this always fits.
    const size_t n = p.size();
    memcpy(out + n, kCut, cutLen + 1);
    return n + cutLen;
  }
  return p.size();
}

} // namespace NomadNet
} // namespace Rns
