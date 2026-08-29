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
#include "RnsTransport.h"
#include "Bootloader.h"

RetiTransportServer transportServer;

// ---------------------------------------------------------------------------
void RetiTransportServer::begin(RingbufHandle_t tcpInRing) {
  _tcpInRing = tcpInRing;
  _lock = xSemaphoreCreateMutex();

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
// The server hands over a client it allocated; one that is turned away has
// to be freed by whoever turned it away, once the close has gone through.
// Closing alone leaked it — both the limit branch and the restart branch
// did — one per connection attempt, for as long as the condition held.
static void refuse(AsyncClient* client) {
  client->onDisconnect([](void*, AsyncClient* c) { delete c; }, nullptr);
  client->close();
}

void RetiTransportServer::onClient(AsyncClient* client) {
  if (client == nullptr) return;

  // A restart is seconds away: a peer accepted now would be registered with
  // Transport and torn down before it exchanged a packet.
  if (Bootloader::pending()) { refuse(client); return; }

  if (clientCount() >= RNS_MAX_CLIENTS) {
    log_w("Rejecting %s: client limit (%d) reached",
          client->remoteIP().toString().c_str(), RNS_MAX_CLIENTS);
    refuse(client);
    return;
  }

  auto* ctx = new ClientCtx{ _nextId++, client, {} };
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

  // Address *and* port, the way RNS's own TCPServerInterface names a spawned
  // interface. Transport identifies an interface by the hash of its name, so
  // the port is what keeps a phone that reconnects from the same address from
  // colliding with the interface its previous socket still holds.
  char remote[46];
  snprintf(remote, sizeof(remote), "%s:%u",
           client->remoteIP().toString().c_str(), (unsigned)client->remotePort());
  RnsTransport::clientConnected(ctx->id, remote);
  log_i("Reticulum peer connected: %s (#%lu, %d total)", remote,
        (unsigned long)ctx->id, (int)g_stats.tcpClients);
}

void RetiTransportServer::onDisconnect(ClientCtx* ctx) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  for (auto it = _clients.begin(); it != _clients.end(); ++it) {
    if (*it == ctx) { _clients.erase(it); break; }
  }
  g_stats.tcpClients = _clients.size();
  xSemaphoreGive(_lock);

  RnsTransport::clientDisconnected(ctx->id);
  AsyncClient* client = ctx->client;
  delete ctx;
  delete client;                         // server-accepted clients are ours
  log_i("Reticulum peer disconnected (%d left)", (int)g_stats.tcpClients);
}

// ---------------------------------------------------------------------------
// Inbound: TCP bytes -> HDLC deframer -> [client id | packet] -> RNS task.
// Runs on the AsyncTCP task (core 0); it only copies bytes.
// ---------------------------------------------------------------------------
void RetiTransportServer::onData(ClientCtx* ctx, const uint8_t* data, size_t len) {
  const uint32_t oversizedBefore = ctx->deframer.oversized();
  for (size_t i = 0; i < len; i++) {
    ctx->deframer.feed(data[i], [this, ctx](const uint8_t* pkt, size_t pktLen) {
      g_stats.tcpRxPackets++;
      uint8_t item[sizeof(RnsTransport::TcpItemHeader) + RNS_MTU];
      RnsTransport::TcpItemHeader h{ ctx->id };
      memcpy(item, &h, sizeof(h));
      memcpy(item + sizeof(h), pkt, pktLen);
      // Drop rather than back-pressure the socket: a stalled AsyncTCP task
      // takes the web server down with it, and Reticulum tolerates loss.
      if (xRingbufferSend(_tcpInRing, item, sizeof(h) + pktLen, pdMS_TO_TICKS(20)) != pdTRUE)
        log_w("TCP-in ring full, dropping %u-byte packet", (unsigned)pktLen);
    });
  }
  // A peer framing packets larger than this node's MTU is indistinguishable
  // from a working one whose traffic never arrives, so say it — once every
  // OVERSIZE_LOG_MS, because a desynced peer produces a stream of them.
  const uint32_t oversized = ctx->deframer.oversized();
  if (oversized != oversizedBefore && millis() - _lastOversizeLogMs >= OVERSIZE_LOG_MS) {
    _lastOversizeLogMs = millis();
    log_w("%s framed a packet over the %d-byte MTU (%lu so far); it will not be received",
          ctx->client->remoteIP().toString().c_str(), RNS_MTU, (unsigned long)oversized);
  }
}

// ---------------------------------------------------------------------------
// Outbound: one packet to one client. Called from the RNS task; _lock
// serialises the client list and the framing scratch buffer.
// ---------------------------------------------------------------------------
bool RetiTransportServer::sendTo(uint32_t clientId, const uint8_t* packet, size_t len) {
  bool ok = false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  size_t frameLen = HDLC::frame(packet, len, _frameBuf, sizeof(_frameBuf));
  if (frameLen > 0) {
    for (auto* ctx : _clients) {
      if (ctx->id != clientId) continue;
      AsyncClient* c = ctx->client;
      // Slow-consumer policy: if the socket's window can't take the whole
      // frame right now, drop it for that client instead of blocking.
      if (c->connected() && c->canSend() && c->space() >= frameLen) {
        c->write((const char*)_frameBuf, frameLen);
        ok = true;
      }
      break;
    }
  }
  xSemaphoreGive(_lock);
  return ok;
}

