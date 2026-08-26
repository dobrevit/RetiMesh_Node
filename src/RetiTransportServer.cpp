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
//  RetiTransportServer.cpp — see RetiTransportServer.h for the data flow.
// ============================================================================
#include "RetiTransportServer.h"

RetiTransportServer transportServer;

// ---------------------------------------------------------------------------
void RetiTransportServer::begin(RingbufHandle_t txRing, RingbufHandle_t rxRing) {
  _txRing = txRing;
  _rxRing = rxRing;
  _lock   = xSemaphoreCreateMutex();

  _server = new AsyncServer(RNS_TCP_PORT);
  _server->setNoDelay(true);
  _server->onClient([](void* self, AsyncClient* client) {
    static_cast<RetiTransportServer*>(self)->onClient(client);
  }, this);
  _server->begin();

  log_i("Reticulum transport listening on 0.0.0.0:%d", RNS_TCP_PORT);
}

size_t RetiTransportServer::clientCount() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  size_t n = _clients.size();
  xSemaphoreGive(_lock);
  return n;
}

// ---------------------------------------------------------------------------
// Connection lifecycle (all callbacks run on the AsyncTCP event task).
// ---------------------------------------------------------------------------
void RetiTransportServer::onClient(AsyncClient* client) {
  if (client == nullptr) return;

  if (clientCount() >= RNS_MAX_CLIENTS) {
    log_w("Rejecting %s: client limit (%d) reached",
          client->remoteIP().toString().c_str(), RNS_MAX_CLIENTS);
    client->close(true);
    return;
  }

  auto* ctx = new ClientCtx{ client, {} };
  client->setNoDelay(true);              // packets are latency-sensitive
  client->setKeepAlive(10000, 3);        // detect vanished phones

  client->onData([](void* arg, AsyncClient*, void* data, size_t len) {
    auto* c = static_cast<ClientCtx*>(arg);
    transportServer.onData(c, (const uint8_t*)data, len);
  }, ctx);

  client->onDisconnect([](void* arg, AsyncClient*) {
    transportServer.onDisconnect(static_cast<ClientCtx*>(arg));
  }, ctx);

  client->onError([](void* arg, AsyncClient*, int8_t error) {
    log_w("TCP client error %d", error);
  }, ctx);

  xSemaphoreTake(_lock, portMAX_DELAY);
  _clients.push_back(ctx);
  g_stats.tcpClients = _clients.size();
  xSemaphoreGive(_lock);

  log_i("Reticulum peer connected: %s (%d total)",
        client->remoteIP().toString().c_str(), (int)g_stats.tcpClients);
}

void RetiTransportServer::onDisconnect(ClientCtx* ctx) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  for (auto it = _clients.begin(); it != _clients.end(); ++it) {
    if (*it == ctx) { _clients.erase(it); break; }
  }
  g_stats.tcpClients = _clients.size();
  xSemaphoreGive(_lock);

  AsyncClient* client = ctx->client;
  delete ctx;
  delete client;                         // server-accepted clients are ours
  log_i("Reticulum peer disconnected (%d left)", (int)g_stats.tcpClients);
}

// ---------------------------------------------------------------------------
// Inbound: TCP bytes -> HDLC deframer -> ring buffer + local relay.
// Runs on the AsyncTCP task (core 0); it only copies bytes, the radio
// work happens on core 1.
// ---------------------------------------------------------------------------
void RetiTransportServer::onData(ClientCtx* ctx, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    ctx->deframer.feed(data[i], [this, ctx](const uint8_t* pkt, size_t pktLen) {
      g_stats.tcpRxPackets++;

      // (a) Queue for RF. If the air is saturated and the ring is full,
      //     drop rather than back-pressure the socket — Reticulum links
      //     tolerate loss, a stalled AsyncTCP task takes down everything.
      if (xRingbufferSend(_txRing, pkt, pktLen, pdMS_TO_TICKS(20)) != pdTRUE) {
        log_w("TX ring full, dropping %u-byte packet", (unsigned)pktLen);
      }

      // (b) Local hub: relay to the other Wi-Fi clients so two phones on
      //     this AP can reach each other without a round-trip over RF.
      //     Never echoed to the sender.
      broadcast(pkt, pktLen, ctx);
    });
  }
}

// ---------------------------------------------------------------------------
// Outbound fan-out. Called from the AsyncTCP task (client relay) and the
// bridge task below (LoRa RX); _lock serializes both the client list and
// the shared framing scratch buffer.
// ---------------------------------------------------------------------------
void RetiTransportServer::broadcast(const uint8_t* packet, size_t len,
                                    ClientCtx* exclude) {
  xSemaphoreTake(_lock, portMAX_DELAY);

  size_t frameLen = HDLC::frame(packet, len, _frameBuf, sizeof(_frameBuf));
  if (frameLen > 0) {
    for (auto* ctx : _clients) {
      if (ctx == exclude) continue;
      AsyncClient* c = ctx->client;
      // Slow-consumer policy: if the socket's window can't take the whole
      // frame right now, drop it for that client instead of blocking.
      if (c->connected() && c->canSend() && c->space() >= frameLen) {
        c->write((const char*)_frameBuf, frameLen);
      }
    }
  }

  xSemaphoreGive(_lock);
}

// ---------------------------------------------------------------------------
// Bridge task — pinned to CORE 1 by main.cpp. Completes the LoRa -> TCP
// path: blocks on the RX ring buffer that radioTask fills with reassembled
// RNS packets, and fans each one out to all connected clients.
// ---------------------------------------------------------------------------
void RetiTransportServer::bridgeTask(void* selfPtr) {
  auto* self = static_cast<RetiTransportServer*>(selfPtr);
  for (;;) {
    size_t itemSize = 0;
    uint8_t* item = (uint8_t*)xRingbufferReceive(self->_rxRing, &itemSize,
                                                 portMAX_DELAY);
    if (item != nullptr) {
      self->broadcast(item, itemSize, nullptr);
      vRingbufferReturnItem(self->_rxRing, item);
    }
  }
}
