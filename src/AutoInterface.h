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
//  AutoInterface.h — RNS AutoInterface peering over the SoftAP (spike)
//
//  Reticulum's AutoInterface needs no addresses: every peer multicasts a
//  discovery token every 1.6 s to an IPv6 link-local group derived from the
//  group id ("reticulum" -> ff12:0:d70b:fb1c:16e4:5e39:485e:31e1), UDP port
//  29716. token = sha256(group_id + peer_link_local_address_as_text). A
//  receiver recomputes it for the sender's address; on a match the sender
//  is a peer for 22 s, and RNS packets flow as UDP unicast to port 42671.
//
//  This spike proves the transport path on the ESP32 SoftAP: the node
//  joins the group, validates incoming tokens, sends its own, and counts
//  data datagrams. The per-peer RNS interfaces come next.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace AutoInterface {

struct Peer {
  char     addr[46];
  uint32_t lastSeenMs;
  uint32_t datagrams;
};

void begin();                     // starts the discovery/data task (core 0)
size_t peers(Peer* out, size_t max);
const char* localAddress();       // our link-local address text, "" until assigned

} // namespace AutoInterface
