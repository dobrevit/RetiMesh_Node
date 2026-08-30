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
//  AutoInterface.h — RNS AutoInterface peering on the access point and the LAN
//
//  Reticulum's AutoInterface needs no addresses: every peer multicasts a
//  discovery token every 1.6 s to an IPv6 link-local group derived from the
//  group id ("reticulum" -> ff12:0:d70b:fb1c:16e4:5e39:485e:31e1), UDP port
//  29716. token = sha256(group_id + peer_link_local_address_as_text). A
//  receiver recomputes it for the sender's address; on a match the sender
//  is a peer for 22 s, and RNS packets flow as UDP unicast to port 42671.
//
//  The same token also goes out unicast to every known peer every 5.2 s, on
//  port 29717 — RNS's reverse peering. It is what keeps peerings alive on a
//  link that drops link-local multicast, which is most consumer access points
//  between wireless clients, and any phone whose radio filters multicast while
//  the screen is off. Multicast alone finds peers; unicast keeps them.
//
//  Peering runs on every Wi-Fi netif that has a link-local address — the
//  node's own access point and the station link onto the LAN — and joins each
//  as it appears, so neither has to be up first or at all.
//
//  Every peer becomes its own RNS interface on the Transport (registered
//  and removed through RnsTransport's event queue, exactly like TCP
//  clients); inbound datagrams go through tcpInRing tagged with the peer
//  id, outbound packets are UDP unicast from the RNS task.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include "Config.h"

namespace AutoInterface {

struct Peer {
  uint32_t id;                    // RnsTransport client id (AUTO_ID_BASE | n)
  char     addr[46];
  int      ifindex;               // netif the peer was last heard on (AP or STA)
  uint32_t lastSeenMs;
  uint32_t datagrams;
};

constexpr uint32_t AUTO_ID_BASE = 0x80000000UL;   // ids above this are AutoInterface peers

void begin(RingbufHandle_t inRing);   // starts the discovery/data task (core 0)
size_t peers(Peer* out, size_t max);
size_t peerCount();
const char* localAddress();           // our link-local on the first joined link, "" until one is
bool enabled();

// Called from the RNS task: one RNS packet as a UDP datagram to one peer.
bool sendTo(uint32_t peerId, const uint8_t* packet, size_t len);

} // namespace AutoInterface
