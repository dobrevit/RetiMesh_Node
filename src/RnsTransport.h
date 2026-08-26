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

namespace RnsTransport {

// Item layout in tcpInRing: uint32_t client id, then the packet bytes.
struct TcpItemHeader { uint32_t clientId; };

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing);
void loop();                               // RNS task only
bool started();

// Posted by the AsyncTCP task; applied on the RNS task.
void clientConnected(uint32_t id, const char* remote);
void clientDisconnected(uint32_t id);

// Thread-safe snapshots, refreshed by the RNS task.
struct PathInfo  { char hash[33]; char via[20]; uint8_t hops; uint32_t ageS; };
struct IfaceInfo { char name[24]; char mode[14]; uint32_t rxb, txb; };
size_t paths(PathInfo* out, size_t max);
size_t interfaces(IfaceInfo* out, size_t max);
size_t pathCount();

const char* modeName(uint8_t mode);        // settings value -> "full", ...

} // namespace RnsTransport
