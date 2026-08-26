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
//  AutoInterface.cpp — see AutoInterface.h
// ============================================================================
#include "AutoInterface.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/ip6_addr.h>
#include <lwip/inet.h>
#include <esp_netif.h>
#include <freertos/semphr.h>
#include <freertos/ringbuf.h>
#include "RnsAnnounce.h"
#include "RnsTransport.h"
#include "Settings.h"
#include "WifiManager.h"

namespace {

char kGroupId[33]        = AUTOIF_GROUP_ID;
char kGroupAddr[48]      = "";                 // derived from the group id at start
const uint16_t kDiscPort = 29716;
const uint16_t kDataPort = 42671;
const uint32_t kAnnounceMs = 1600;
const uint32_t kPeerTimeoutMs = 22000;

int  sDisc = -1, sData = -1;
int  sIfIndex = 0;                     // AP netif: multicast sends + join
int  sStaIfIndex = 0;                  // STA netif when station mode is up (joined too)
bool sStaJoined = false;
char sLocal[46] = "";
bool sEnabled = false;
SemaphoreHandle_t sLock;
RingbufHandle_t sInRing = nullptr;
AutoInterface::Peer sPeers[AUTOIF_MAX_PEERS];
uint32_t sNextPeer = 1;

// ff + type '1' (temporary) + scope '2' (link) + ":0:" + six 16-bit words
// from bytes 2..13 of sha256(group id) — RNS.Interfaces.AutoInterface.
void deriveGroupAddress() {
  uint8_t g[32];
  Rns::sha256((const uint8_t*)kGroupId, strlen(kGroupId), g);
  snprintf(kGroupAddr, sizeof(kGroupAddr), "ff12:0:%x:%x:%x:%x:%x:%x",
           g[3] + (g[2] << 8), g[5] + (g[4] << 8), g[7] + (g[6] << 8),
           g[9] + (g[8] << 8), g[11] + (g[10] << 8), g[13] + (g[12] << 8));
}

// RNS hashes the RFC 5952 text of the address: lowercase, compressed, no
// scope suffix. lwIP prints uppercase, so normalise before hashing/logging.
void lowercase(char* s) { for (; *s; s++) if (*s >= 'A' && *s <= 'F') *s += 'a' - 'A'; }

void token(const char* addrText, uint8_t out[32]) {
  std::string m = std::string(kGroupId) + addrText;
  Rns::sha256((const uint8_t*)m.data(), m.size(), out);
}

// Our link-local address on the AP netif, RFC 5952 text (lwIP compresses
// the same way Python's inet_ntop does).
bool localLinkLocal(char* out, size_t cap) {
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) return false;
  esp_ip6_addr_t ip6;
  if (esp_netif_get_ip6_linklocal(ap, &ip6) != ESP_OK) return false;
  ip6_addr_t a; memcpy(a.addr, ip6.addr, 16);
#if LWIP_IPV6_SCOPES
  a.zone = 0;
#endif
  strlcpy(out, ip6addr_ntoa(&a), cap);
  lowercase(out);
  return out[0] != '\0';
}

// Returns the peer id; registers a new peer with Transport.
uint32_t notePeer(const char* addr, bool data, int ifindex = 0) {
  xSemaphoreTake(sLock, portMAX_DELAY);
  AutoInterface::Peer* slot = nullptr;
  for (auto& p : sPeers) if (p.addr[0] && strcmp(p.addr, addr) == 0) { slot = &p; break; }
  bool fresh = false;
  if (!slot) {
    for (auto& p : sPeers) if (!p.addr[0]) { slot = &p; break; }
    if (!slot) { xSemaphoreGive(sLock); return 0; }         // table full: ignore until one expires
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->addr, addr, sizeof(slot->addr));
    slot->id = AutoInterface::AUTO_ID_BASE | sNextPeer++;
    slot->ifindex = ifindex ? ifindex : sIfIndex;
    fresh = true;
  }
  slot->lastSeenMs = millis();
  if (data) slot->datagrams++;
  uint32_t id = slot->id;
  xSemaphoreGive(sLock);
  if (fresh) {
    log_i("AutoInterface: new peer %s (#%lu)", addr, (unsigned long)(id & 0xFFFF));
    RnsTransport::clientConnected(id, addr);
  }
  return id;
}

void expirePeers() {
  uint32_t expired[AUTOIF_MAX_PEERS]; size_t n = 0;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers)
    if (p.addr[0] && millis() - p.lastSeenMs > kPeerTimeoutMs) {
      log_i("AutoInterface: peer %s timed out", p.addr);
      expired[n++] = p.id; p.addr[0] = '\0';
    }
  xSemaphoreGive(sLock);
  for (size_t i = 0; i < n; i++) RnsTransport::clientDisconnected(expired[i]);
}

bool peerAddress(uint32_t id, sockaddr_in6& out) {
  bool ok = false;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0] && p.id == id) {
    memset(&out, 0, sizeof(out));
    out.sin6_family = AF_INET6; out.sin6_port = htons(kDataPort);
    inet_pton(AF_INET6, p.addr, &out.sin6_addr);
#if LWIP_IPV6_SCOPES
    out.sin6_scope_id = p.ifindex ? p.ifindex : sIfIndex;
#endif
    ok = true; break;
  }
  xSemaphoreGive(sLock);
  return ok;
}

bool openSockets() {
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  sIfIndex = ap ? esp_netif_get_netif_impl_index(ap) : 0;

  sDisc = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  sData = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (sDisc < 0 || sData < 0) { log_e("AutoInterface: socket() failed"); return false; }
  int one = 1;
  setsockopt(sDisc, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in6 any = {}; any.sin6_family = AF_INET6; any.sin6_addr = in6addr_any;
  any.sin6_port = htons(kDiscPort);
  if (bind(sDisc, (sockaddr*)&any, sizeof(any)) < 0) { log_e("AutoInterface: bind %u failed (errno %d)", kDiscPort, errno); return false; }
  any.sin6_port = htons(kDataPort);
  if (bind(sData, (sockaddr*)&any, sizeof(any)) < 0) { log_e("AutoInterface: bind %u failed (errno %d)", kDataPort, errno); return false; }

  ipv6_mreq mreq = {};
  inet_pton(AF_INET6, kGroupAddr, &mreq.ipv6mr_multiaddr);
  mreq.ipv6mr_interface = sIfIndex;
  if (setsockopt(sDisc, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
    log_e("AutoInterface: IPV6_JOIN_GROUP failed (errno %d)", errno); return false;
  }
  setsockopt(sDisc, IPPROTO_IPV6, IPV6_MULTICAST_IF, &sIfIndex, sizeof(sIfIndex));
  timeval tv = { 0, 200000 };
  setsockopt(sDisc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sData, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  log_i("AutoInterface: listening on [%s]:%u (disc) and :%u (data), ifindex %d", kGroupAddr, kDiscPort, kDataPort, sIfIndex);
  return true;
}

// Join the group on the station netif once it exists (station mode), so
// LAN peers are discovered as well as AP clients. Their tokens are checked
// against our STA link-local address, ours are sent on both interfaces.
char sStaLocal[46] = "";
void maybeJoinStation() {
  if (sStaJoined) return;
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) return;
  esp_ip6_addr_t ip6;
  if (esp_netif_get_ip6_linklocal(sta, &ip6) != ESP_OK) return;
  ip6_addr_t a; memcpy(a.addr, ip6.addr, 16);
#if LWIP_IPV6_SCOPES
  a.zone = 0;
#endif
  strlcpy(sStaLocal, ip6addr_ntoa(&a), sizeof(sStaLocal)); lowercase(sStaLocal);
  sStaIfIndex = esp_netif_get_netif_impl_index(sta);
  ipv6_mreq mreq = {};
  inet_pton(AF_INET6, kGroupAddr, &mreq.ipv6mr_multiaddr);
  mreq.ipv6mr_interface = sStaIfIndex;
  if (setsockopt(sDisc, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
    log_w("AutoInterface: station IPV6_JOIN_GROUP failed (errno %d)", errno); return;
  }
  sStaJoined = true;
  log_i("AutoInterface: also listening on the station network, link-local %s", sStaLocal);
}

void sendDiscoveryOn(int ifindex, const char* localText) {
  if (!localText[0]) return;
  uint8_t tok[32]; token(localText, tok);
  sockaddr_in6 dst = {}; dst.sin6_family = AF_INET6; dst.sin6_port = htons(kDiscPort);
  inet_pton(AF_INET6, kGroupAddr, &dst.sin6_addr);
#if LWIP_IPV6_SCOPES
  dst.sin6_scope_id = ifindex;
#endif
  setsockopt(sDisc, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
  if (sendto(sDisc, tok, sizeof(tok), 0, (sockaddr*)&dst, sizeof(dst)) < 0)
    log_w("AutoInterface: discovery sendto failed (errno %d)", errno);
}

void sendDiscovery() {
  sendDiscoveryOn(sIfIndex, sLocal);
  if (sStaJoined) sendDiscoveryOn(sStaIfIndex, sStaLocal);
}

void task(void*) {
  deriveGroupAddress();
  // IPv6 on the AP comes up asynchronously; wait for a link-local address.
  WiFi.softAPenableIpV6();
  for (int i = 0; i < 100 && !localLinkLocal(sLocal, sizeof(sLocal)); i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (!sLocal[0]) { log_w("AutoInterface: no link-local IPv6 on the AP, giving up"); vTaskDelete(nullptr); return; }
  log_i("AutoInterface: AP link-local %s", sLocal);
  if (!openSockets()) { vTaskDelete(nullptr); return; }
  sEnabled = true;

  uint32_t lastAnnounce = 0, lastExpire = 0;
  uint8_t buf[1500];
  for (;;) {
    uint32_t now = millis();
    if (now - lastAnnounce >= kAnnounceMs) { lastAnnounce = now; sendDiscovery(); }
    if (now - lastExpire >= 1000) { lastExpire = now; expirePeers(); if (wifiManager.stationConnected()) maybeJoinStation(); }

    sockaddr_in6 src; socklen_t sl = sizeof(src);
    int n = recvfrom(sDisc, buf, sizeof(buf), 0, (sockaddr*)&src, &sl);
    if (n > 0) {
      char addr[46]; inet_ntop(AF_INET6, &src.sin6_addr, addr, sizeof(addr)); lowercase(addr);
      if (strcmp(addr, sLocal) != 0 && strcmp(addr, sStaLocal) != 0) {
        uint8_t expect[32]; token(addr, expect);
        if (n == 32 && memcmp(expect, buf, 32) == 0) {
          int ifx = 0;
#if LWIP_IPV6_SCOPES
          ifx = src.sin6_scope_id;
#endif
          notePeer(addr, false, ifx);
        }
        else log_w("AutoInterface: discovery datagram (%d bytes) from %s with a non-matching token", n, addr);
      }
    }
    sl = sizeof(src);
    n = recvfrom(sData, buf, sizeof(buf), 0, (sockaddr*)&src, &sl);
    if (n > 0 && n <= RNS_MTU) {
      char addr[46]; inet_ntop(AF_INET6, &src.sin6_addr, addr, sizeof(addr)); lowercase(addr);
      uint32_t id = notePeer(addr, true);
      if (id) {
        // Same ring and layout as the TCP clients: [client id | packet]
        uint8_t item[sizeof(RnsTransport::TcpItemHeader) + RNS_MTU];
        RnsTransport::TcpItemHeader h{ id };
        memcpy(item, &h, sizeof(h));
        memcpy(item + sizeof(h), buf, n);
        if (xRingbufferSend(sInRing, item, sizeof(h) + n, pdMS_TO_TICKS(20)) != pdTRUE)
          log_w("AutoInterface: inbound ring full, dropping %d bytes", n);
      }
    }
  }
}

} // namespace

namespace AutoInterface {

void begin(RingbufHandle_t inRing) {
  sLock = xSemaphoreCreateMutex();
  sInRing = inRing;
  memset(sPeers, 0, sizeof(sPeers));
  strlcpy(kGroupId, settings.transport().autoGroupId[0] ? settings.transport().autoGroupId : AUTOIF_GROUP_ID, sizeof(kGroupId));
  if (!settings.transport().autoEnabled) { log_i("AutoInterface disabled in settings"); return; }
  xTaskCreatePinnedToCore(task, "autoif", 6144, nullptr, 2, nullptr, 0);
}

bool enabled() { return sEnabled; }

size_t peerCount() {
  size_t k = 0;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0]) k++;
  xSemaphoreGive(sLock);
  return k;
}

bool sendTo(uint32_t peerId, const uint8_t* packet, size_t len) {
  if (sData < 0) return false;
  sockaddr_in6 dst;
  if (!peerAddress(peerId, dst)) return false;
  int n = sendto(sData, packet, len, 0, (sockaddr*)&dst, sizeof(dst));
  if (n < 0) { log_w("AutoInterface: sendto failed (errno %d)", errno); return false; }
  return true;
}

size_t peers(Peer* out, size_t max) {
  size_t k = 0;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0] && k < max) out[k++] = p;
  xSemaphoreGive(sLock);
  return k;
}

const char* localAddress() { return sLocal; }

} // namespace AutoInterface
