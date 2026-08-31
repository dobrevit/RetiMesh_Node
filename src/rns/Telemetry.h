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

#include "MsgPack.h"

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

  const char* information = nullptr;   // firmware and board, in a line

  bool     haveMemory = false;
  uint64_t heapCapacity = 0, heapUsed = 0;
  bool     haveStorage = false;
  uint64_t flashCapacity = 0, flashUsed = 0;
  bool     haveProcessor = false;
  uint64_t cpuHz = 0;
};

// How many sensors a snapshot will produce. Written before the map header,
// which has to say how many pairs follow — a header promising more than
// arrives is a document nothing can read.
inline size_t sensorCount(const Snapshot& s) {
  size_t n = 0;
  if (s.utc)           n++;
  if (s.information)   n++;
  if (s.haveBattery)   n++;
  if (s.havePosition)  n++;
  if (s.haveSignal)    n++;
  if (s.haveProcessor) n++;
  if (s.haveMemory)    n++;
  if (s.haveStorage)   n++;
  return n;
}

// The telemetry document: {sensor id: reading}. Returns bytes written, or 0 if
// it did not fit — never a partial document, because half a msgpack map is not
// a smaller telemetry reading, it is a parse error at the far end.
inline size_t build(const Snapshot& s, uint8_t* out, size_t cap) {
  MsgPack w(out, cap);
  w.map(sensorCount(s));

  // Time, which Sideband's own Telemeter always includes.
  if (s.utc) w.uint(kSidTime).uint(s.utc);

  // A line about what this is. str, not bin: Sideband stores it with str()
  // and renders it as it stands, so bytes would show as b'...'.
  if (s.information) w.uint(kSidInformation).str(s.information);

  // [percent, charging, temperature]. Charging is nil where the board cannot
  // answer it, rather than a false that reads as "plugged in and not taking
  // charge" (Power.h). No temperature sensor on any board here yet.
  if (s.haveBattery) {
    w.uint(kSidBattery).array(3).real(s.batteryPercent);
    if (s.chargeKnown) w.boolean(s.charging); else w.nil();
    w.nil();
  }

  // Fixed point, big-endian, as *bytes* — struct.pack("!i", …) on the other
  // side. Bearing is zero because nothing here measures a heading, and a node
  // on a hill has none; sending zero is the same claim as standing still.
  if (s.havePosition) {
    w.uint(kSidLocation).array(7)
     .binI32((int32_t)(s.latitude * 1e6))
     .binI32((int32_t)(s.longitude * 1e6))
     .binI32((int32_t)(s.altitudeM * 1e2))
     .binU32((uint32_t)(s.speedKmh * 1e2))
     .binI32(0)
     .binU16((uint16_t)(s.accuracyM * 1e2))
     .uint(s.positionAt);
  }

  // [rssi, snr, quality] — what this node heard of the message that asked.
  if (s.haveSignal) {
    w.uint(kSidPhysicalLink).array(3).real(s.rssi).real(s.snr).uint(s.quality);
  }

  // These three share a shape: a list of [label, [capacity, used]]. Label 0
  // is the only one this node has of each.
  auto capacityUsed = [&](uint32_t sid, uint64_t capacity, uint64_t used) {
    w.uint(sid).array(1).array(2).uint(0).array(2).uint(capacity).uint(used);
  };
  if (s.haveProcessor) capacityUsed(kSidProcessor, s.cpuHz, 0);
  if (s.haveMemory)    capacityUsed(kSidRam, s.heapCapacity, s.heapUsed);
  if (s.haveStorage)   capacityUsed(kSidNvm, s.flashCapacity, s.flashUsed);

  return w.ok() ? w.size() : 0;
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
