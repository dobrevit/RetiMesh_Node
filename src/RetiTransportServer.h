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
//  RetiTransportServer.h — Reticulum clients over TCP port 4242
//
//  Speaks the exact wire format of RNS.Interfaces.TCPInterface (HDLC
//  framing, see HDLC.h), so any stock Reticulum client — Sideband on a
//  phone, rnsd on a laptop — connects with a plain TCPClientInterface:
//
//      [[RetiMesh Gateway]]
//        type = TCPClientInterface
//        enabled = yes
//        target_host = 10.42.0.1
//        target_port = 4242
//
//  This class only moves bytes. Every client is registered with the
//  Reticulum Transport as its own interface (RnsTransport.h), which
//  decides what gets forwarded where — exactly like rnsd's
//  TCPServerInterface spawning one interface per connection.
//
//    client -> onData -> HDLC deframe -> tcpInRing [id | packet] -> RNS task
//    RNS task -> sendTo(id, packet) -> HDLC frame -> client socket
// ============================================================================
#pragma once

#include <Arduino.h>
#include <AsyncTCP.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/ringbuf.h>
#include <vector>
#include "Config.h"
#include "HDLC.h"

class RetiTransportServer {
public:
  // Starts listening on 0.0.0.0:RNS_TCP_PORT. Deframed packets go to
  // tcpInRing, prefixed with the client id (RnsTransport::TcpItemHeader).
  void begin(RingbufHandle_t tcpInRing);

  size_t clientCount();

  // Called from the RNS task: frames and writes one packet to one client.
  bool sendTo(uint32_t clientId, const uint8_t* packet, size_t len);

  // Verifies an announce (hash chain + Ed25519) and records the sender in
  // the neighbour table. Called by the RNS interfaces for metadata only;
  // Transport does its own validation.
  static void noteAnnounce(const uint8_t* raw, size_t len, bool viaWifi);

private:
  struct ClientCtx {
    uint32_t       id;
    AsyncClient*   client;
    HDLC::Deframer deframer;             // per-connection stream state
  };

  void onClient(AsyncClient* client);
  void onData(ClientCtx* ctx, const uint8_t* data, size_t len);
  void onDisconnect(ClientCtx* ctx);

  AsyncServer*            _server = nullptr;
  RingbufHandle_t         _tcpInRing = nullptr;
  std::vector<ClientCtx*> _clients;
  SemaphoreHandle_t       _lock   = nullptr;   // guards _clients + _frameBuf
  uint32_t                _nextId = 1;

  // Shared framing scratch: worst case 2 + 2*508 bytes.
  uint8_t _frameBuf[HDLC::frameCapacity(2 * LORA_FRAG_PAYLOAD)];
};

extern RetiTransportServer transportServer;
