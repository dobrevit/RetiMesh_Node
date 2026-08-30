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
//  RnsTransport.h — the Reticulum stack (microReticulum) and its interfaces
//
//  The node is a real RNS Transport instance. Two interface types plug our
//  hardware paths into it, each with its own RNS interface mode
//  (full / gateway / access point / roaming / boundary — exactly rnsd's):
//
//    LoRaRnsInterface        one instance. send_outgoing -> TX ring buffer
//                            (radioTask fragments + transmits); loop() drains
//                            the RX ring buffer -> handle_incoming.
//    TcpClientRnsInterface   one instance per connected Wi-Fi client, like
//                            RNS's TCPServerInterface spawns. send_outgoing
//                            -> HDLC -> that client's socket; inbound bytes
//                            arrive through the TCP-in ring buffer.
//
//  Threading: microReticulum is single-threaded. Every call into it happens
//  on the RNS task (core 1) via loop(); the AsyncTCP task only posts
//  connect/disconnect events and ring-buffer items.
//
//  Packet flow (Transport decides forwarding, per RNS rules and modes):
//
//    LoRa RF -> radioTask -> rxRing -> LoRaRnsInterface -> Transport::inbound
//    TCP client -> deframe -> tcpInRing -> TcpClientRnsInterface -> inbound
//    Transport::outbound -> interface.send_outgoing -> txRing / socket
// ============================================================================
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include "Config.h"                        // INTERFACE_NAME_MAX, RNS_MAX_INTERFACES

namespace RnsTransport {

// Item layout in tcpInRing: uint32_t client id, then the packet bytes.
struct TcpItemHeader { uint32_t clientId; };

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing);
void loop();                               // RNS task only
bool started();

// Silence microReticulum's log, or restore it. It prints through Serial,
// and while PPP owns the serial port (PppUart.h) a log line lands between
// frames. Only the RNS task may call into the library, so the request is
// recorded here and applied by loop() at its next pass.
void muteLog(bool mute);

// Posted by the AsyncTCP task; applied on the RNS task.
void clientConnected(uint32_t id, const char* remote);
void clientDisconnected(uint32_t id);

// Thread-safe snapshots, refreshed by the RNS task. The name buffers hold a
// whole interface name: they are what a path's "via" is matched against by
// eye, and a truncated one makes two different peers read as the same.
struct PathInfo  { char hash[33]; char via[INTERFACE_NAME_MAX]; uint8_t hops; uint32_t ageS; };
struct IfaceInfo { char name[INTERFACE_NAME_MAX]; char mode[14]; uint32_t rxb, txb; };
size_t paths(PathInfo* out, size_t max);
size_t interfaces(IfaceInfo* out, size_t max);
size_t pathCount();
size_t interfaceCount();                   // whole list, even when a caller reads fewer

// Reticulum table sizes, refreshed alongside the snapshots above. These are the
// structures that grow with traffic, so a soak run watches them as closely as
// it watches the heap: a table that climbs and never falls is where a week-long
// run runs out of memory.
struct Tables {
  uint32_t paths;          // stored path table (microStore-backed)
  uint32_t links;          // link table
  uint32_t activeLinks;
  uint32_t pendingLinks;
  uint32_t destinations;   // local destinations registered with Transport
  uint32_t announces;      // announce table awaiting retransmission
  uint32_t heldAnnounces;
  uint32_t rates;          // per-destination announce rate table
};
Tables tables();

const char* modeName(uint8_t mode);        // settings value -> "full", ...

// Where the microStore files live: "sd" or "littlefs" (chosen at boot).
const char* storageBackend();
const char* storagePath();                 // VFS path, e.g. "/sd/rns"

// The node's LXMF side: what has arrived at its delivery address, and the
// last one of it. Not an inbox — enough that a node which advertises an LXMF
// address can be seen to be answering, and that a message refused for want of
// a verifiable sender is visible as refused rather than as silence.
struct LxmfState {
  uint32_t received;                     // verified, proved, and shown here
  uint32_t rejected;                     // unparseable, unknown sender, or bad signature
  char     lastFrom[33];                 // source hash, hex; empty if none
  char     lastText[121];
  uint32_t lastAgoMs;                    // 0 when nothing has arrived
};
LxmfState lxmf();

} // namespace RnsTransport
