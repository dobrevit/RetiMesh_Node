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
//  RetiTransportServer.h — raw Reticulum transport on TCP port 4242
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
//  The node is a *transparent bridge*, not an RNS Transport instance: it
//  never parses, decrypts or routes packet contents. End-to-end encryption
//  stays entirely between the connected peers. Routing intelligence
//  (announces, path requests) lives in the RNS stacks of the clients.
//
//  Packet flow:
//
//    TCP client A ──HDLC──► onData (AsyncTCP task, core 0)
//                              │ deframe
//                              ├──► TX ring buffer ──► radioTask ──► LoRa RF
//                              └──► other TCP clients (local hub relay)
//
//    LoRa RF ──► radioTask ──► RX ring buffer ──► bridgeTask (core 1)
//                                                    │ HDLC frame
//                                                    └──► every TCP client
//
//  LoRa-received packets are never re-transmitted on LoRa, and client
//  packets are never echoed back to their sender, so the bridge cannot
//  create RF loops.
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
  // Starts listening on 0.0.0.0:RNS_TCP_PORT. annRing receives copies of
  // announces sent by Wi-Fi clients, parsed by the bridge task.
  void begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t annRing);

  size_t clientCount();

  // FreeRTOS entry point — created pinned to core 1 from main.cpp.
  // Drains the RX ring (LoRa -> TCP direction) and broadcasts.
  static void bridgeTask(void* self);

private:
  struct ClientCtx {
    AsyncClient*   client;
    HDLC::Deframer deframer;             // per-connection stream state
  };

  void onClient(AsyncClient* client);
  void onData(ClientCtx* ctx, const uint8_t* data, size_t len);
  void onDisconnect(ClientCtx* ctx);

  static void noteAnnounce(const uint8_t* raw, size_t len, bool viaWifi);

public:
  // HDLC-frames `packet` and writes it to every connected client except
  // `exclude` (nullptr = all). Serialized by _lock; safe from any task.
  void broadcast(const uint8_t* packet, size_t len, ClientCtx* exclude);
private:

  AsyncServer*            _server = nullptr;
  RingbufHandle_t         _txRing = nullptr;   // TCP  -> LoRa
  RingbufHandle_t         _rxRing = nullptr;   // LoRa -> TCP
  RingbufHandle_t         _annRing = nullptr;  // TCP announces -> bridge task (parse only)
  std::vector<ClientCtx*> _clients;
  SemaphoreHandle_t       _lock   = nullptr;   // guards _clients + _frameBuf

  // Shared framing scratch: worst case 2 + 2*508 bytes.
  uint8_t _frameBuf[HDLC::frameCapacity(2 * LORA_FRAG_PAYLOAD)];
};

extern RetiTransportServer transportServer;
