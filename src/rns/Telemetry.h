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
//  Telemetry.h — what this node can honestly say about itself, in the shape
//  Sideband already reads
//
//  A client asks for telemetry with a command (LxmfCommands.h) and gets back
//  an ordinary message with a FIELD_TELEMETRY map in its fields: sensor id to
//  reading. Sideband stores it, plots it over time and puts the node on its
//  map, none of which needs anything installed at either end.
//
//  That is worth having because of what this node is. It sits outdoors on a
//  battery, often somewhere nobody visits, and the questions its operator
//  actually has — is it charging, where is it, is it still hearing anyone, how
//  much heap is left after a fortnight — are exactly the ones these fields
//  carry. The alternative is a portal that has to be reachable, which the node
//  on the hill is not.
//
//  The sensor ids and the shape of each reading are Sideband's; they are not
//  ours to choose, and a value in the wrong shape is not rendered wrongly, it
//  is silently dropped. Two in particular:
//
//    - A position is packed as *bytes*, big-endian fixed point, not as
//      numbers: struct.pack("!i", lat*1e6) on the other side.
//    - Information is msgpack str, where an announce display name is bin.
//      Same project, opposite answers, and both silently wrong the other way.
//
//  Nothing here is claimed that the board cannot answer. A node with no cell
//  sends no battery, one with no receiver sends no position, and a board that
//  cannot see its charger sends nil for charging rather than false — Power.h
//  makes that distinction and it survives to the wire, because "not charging"
//  sends somebody looking for a fault in a working cable.
//
//  Pure, so what a node says about itself can be checked against the real
//  Telemeter on a host rather than by watching a phone.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "MsgPackWriter.h"

namespace Rns {
namespace Telemetry {

// Sideband's sensor ids, and only the ones this node fills.
static const uint32_t kSidTime         = 0x01;
static const uint32_t kSidLocation     = 0x02;
static const uint32_t kSidBattery      = 0x04;
static const uint32_t kSidPhysicalLink = 0x05;
static const uint32_t kSidInformation  = 0x0F;
static const uint32_t kSidProcessor    = 0x13;
static const uint32_t kSidRam          = 0x14;
static const uint32_t kSidNvm          = 0x15;

// The LXMF field the map travels in.
static const uint32_t kFieldTelemetry = 0x02;

// Everything the node knows about itself at one moment, gathered by the caller
// so this file touches no hardware and no clock. A member left as it is here
// is one this board cannot answer, and is left out of the document rather than
// filled in with a zero — a fabricated reading is worse than a missing one,
// because a missing one is visibly missing.
struct Snapshot {
  uint64_t utc = 0;                    // seconds since the epoch; 0 if the node has no idea

  bool     haveBattery = false;
  float    batteryPercent = 0.0f;
  bool     charging = false;
  bool     chargeKnown = false;        // false means the board cannot see its charger

  bool     havePosition = false;
  double   latitude = 0.0, longitude = 0.0;
  float    altitudeM = 0.0f;
  float    speedKmh = 0.0f;
  float    accuracyM = 0.0f;
  uint64_t positionAt = 0;             // when the fix was taken

  bool     haveSignal = false;
  float    rssi = 0.0f, snr = 0.0f;
  uint8_t  quality = 0;

  // Carried, not pointed at. A pointer here aliased one function-local buffer
  // across every snapshot ever taken, so holding two at once silently gave
  // both the second one's text.
  char     information[64] = "";

  bool     haveMemory = false;
  uint64_t heapCapacity = 0, heapUsed = 0;
  bool     haveStorage = false;
  uint64_t flashCapacity = 0, flashUsed = 0;
  bool     haveProcessor = false;
  uint64_t cpuHz = 0;
  bool     haveCard = false;           // a card the node keeps its store on
  uint64_t cardCapacity = 0, cardUsed = 0;
};

// The telemetry document: {sensor id: reading}. Returns bytes written, or 0 if
// it did not fit — never a partial document, because half a msgpack map is not
// a smaller telemetry reading, it is a parse error at the far end.
// The map header says how many pairs follow, and a header that disagrees with
// the body is a document nothing can read — not a shorter reading, a parse
// failure that takes the whole message with it. So the members are written
// first and counted as they go, and the header is written afterwards into the
// byte reserved for it. There is no second list of conditions to keep in step.
//
// One byte is always enough: a fixmap holds fifteen and this node has eight
// sensors. The count is checked anyway, because that is the assumption the
// single reserved byte rests on.
inline size_t build(const Snapshot& s, uint8_t* out, size_t cap) {
  if (cap < 1) return 0;
  MsgPack w(out + 1, cap - 1);
  size_t n = 0;
  auto sensor = [&](uint32_t sid) -> MsgPack& { n++; return w.uint(sid); };

  // Time, which Sideband's own Telemeter always includes. Absent where the
  // node's clock was never set: it places every other reading, so a wrong one
  // is worse than none (see the caller).
  if (s.utc) sensor(kSidTime).uint(s.utc);

  // A line about what this is. str, not bin: Sideband stores it with str()
  // and renders it as it stands, so bytes would show as b'...'.
  if (s.information[0]) sensor(kSidInformation).str(s.information);

  // [percent, charging, temperature]. Charging is nil where the board cannot
  // answer it, rather than a false that reads as "plugged in and not taking
  // charge" (Power.h). No temperature sensor on any board here yet.
  if (s.haveBattery) {
    sensor(kSidBattery).array(3).real(s.batteryPercent);
    if (s.chargeKnown) w.boolean(s.charging); else w.nil();
    w.nil();
  }

  // Fixed point, big-endian, as *bytes* — struct.pack("!i", …) on the other
  // side. Bearing is zero because nothing here measures a heading, and a node
  // on a hill has none; sending zero is the same claim as standing still.
  if (s.havePosition) {
    sensor(kSidLocation).array(7)
     .binI32(MsgPack::clampI32(s.latitude * 1e6))
     .binI32(MsgPack::clampI32(s.longitude * 1e6))
     .binI32(MsgPack::clampI32((double)s.altitudeM * 1e2))
     .binU32(MsgPack::clampU32((double)s.speedKmh * 1e2))
     .binI32(0)
     .binU16(MsgPack::clampU16((double)s.accuracyM * 1e2))
     .uint(s.positionAt);
  }

  // [rssi, snr, quality] — what this node heard of the message that asked.
  if (s.haveSignal) {
    sensor(kSidPhysicalLink).array(3).real(s.rssi).real(s.snr).uint(s.quality);
  }

  // These three share a shape: a list of [label, [capacity, used]]. Label 0
  // is the only one this node has of each.
  auto entry = [&](uint32_t label, uint64_t capacity, uint64_t used) {
    w.array(2).uint(label).array(2).uint(capacity).uint(used);
  };
  if (s.haveProcessor) { sensor(kSidProcessor).array(1); entry(0, s.cpuHz, 0); }
  if (s.haveMemory)    { sensor(kSidRam).array(1);       entry(0, s.heapCapacity, s.heapUsed); }

  // Storage is the one that can be two things. A node keeping its store on a
  // card has an internal flash that never moves and a card that fills, and
  // reporting only the first showed an operator a figure that could not
  // change. The format takes a list, so both go.
  if (s.haveStorage || s.haveCard) {
    const size_t parts = (s.haveStorage ? 1u : 0u) + (s.haveCard ? 1u : 0u);
    sensor(kSidNvm).array(parts);
    if (s.haveStorage) entry(0, s.flashCapacity, s.flashUsed);
    if (s.haveCard)    entry(1, s.cardCapacity, s.cardUsed);
  }

  if (n > 15 || !w.ok()) return 0;          // a fixmap is all the reserved byte holds
  out[0] = (uint8_t)(0x80 | n);
  return 1 + w.size();
}

// The whole fields map an outgoing message carries when it answers a telemetry
// request: {FIELD_TELEMETRY: <document>}. One entry today; a message that also
// carried something else would say so here.
inline size_t fields(const Snapshot& s, uint8_t* out, size_t cap) {
  // The document is written straight into the caller's buffer after the two
  // bytes of map and key, so there is no second copy of it anywhere.
  MsgPack head(out, cap);
  head.map(1).uint(kFieldTelemetry);
  if (!head.ok()) return 0;
  const size_t n = build(s, out + head.size(), cap - head.size());
  return n ? head.size() + n : 0;
}

} // namespace Telemetry
} // namespace Rns
