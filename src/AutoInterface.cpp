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
#include <fcntl.h>
#include <sys/select.h>
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
const uint16_t kUniPort  = 29716 + 1;          // RNS: discovery_port + 1
const uint16_t kDataPort = 42671;
const uint32_t kAnnounceMs = 1600;
const uint32_t kReverseMs  = 5200;             // RNS: announce_interval * 3.25
const uint32_t kPeerTimeoutMs = 22000;
const uint32_t kSelectMs = 200;

int  sDisc = -1, sUni = -1, sData = -1;
bool sEnabled = false;
SemaphoreHandle_t sLock;
RingbufHandle_t sInRing = nullptr;
AutoInterface::Peer sPeers[AUTOIF_MAX_PEERS];
uint32_t sNextPeer = 1;

// The Wi-Fi links peering can run over. Both are optional and either can
// appear late: the access point's IPv6 comes up asynchronously after the AP
// starts, the station's only once it has associated and been given a
// link-local. This used to be two sets of variables and a boolean, anchored on
// the access point — the task gave up altogether if the AP had no link-local
// inside ten seconds, which took the station link down with it even though the
// LAN is the whole point of peering with the other nodes on it.
struct Link {
  const char* key;                  // esp_netif if-key
  const char* what;                 // for the log
  int         ifindex = 0;
  char        local[46] = "";       // our link-local on it, RFC 5952 text
  bool        joined = false;
};
Link sLinks[] = {
  { "WIFI_AP_DEF",  "the access point" },
  { "WIFI_STA_DEF", "the station network" },
};

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

// A netif's link-local address as RFC 5952 text, "" if it has none yet.
bool linkLocalOf(const char* ifkey, char* out, size_t cap) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey(ifkey);
  if (!netif) return false;
  esp_ip6_addr_t ip6;
  if (esp_netif_get_ip6_linklocal(netif, &ip6) != ESP_OK) return false;
  ip6_addr_t a; memcpy(a.addr, ip6.addr, 16);
#if LWIP_IPV6_SCOPES
  a.zone = 0;
#endif
  strlcpy(out, ip6addr_ntoa(&a), cap);
  lowercase(out);
  return out[0] != '\0';
}

bool isOurAddress(const char* addr) {
  for (const Link& l : sLinks) if (l.joined && strcmp(addr, l.local) == 0) return true;
  return false;
}

const Link* linkByIndex(int ifindex) {
  for (const Link& l : sLinks) if (l.joined && l.ifindex == ifindex) return &l;
  return nullptr;
}

// Join the discovery group on any link that has come up and is not joined
// yet. Called at start and once a second afterwards, so a station that
// associates minutes after boot is peered on without a restart.
void refreshLinks() {
  for (Link& l : sLinks) {
    if (l.joined) continue;
    if (!linkLocalOf(l.key, l.local, sizeof(l.local))) continue;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(l.key);
    l.ifindex = netif ? esp_netif_get_netif_impl_index(netif) : 0;
    ipv6_mreq mreq = {};
    inet_pton(AF_INET6, kGroupAddr, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = l.ifindex;
    if (setsockopt(sDisc, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
      log_w("AutoInterface: could not join the group on %s (errno %d)", l.what, errno);
      l.local[0] = '\0';
      continue;
    }
    l.joined = true;
    log_i("AutoInterface: peering on %s, link-local %s (ifindex %d)", l.what, l.local, l.ifindex);
  }
}

// Returns the peer id, registering a new peer with Transport. Only a peering
// token creates one: a data datagram from an address that has not proved it
// knows the group id is not evidence of a peer, and RNS reads it the same way
// (AutoInterface.process_incoming ignores senders it has not peered with).
uint32_t touchPeer(const char* addr, bool create, bool data, int ifindex) {
  xSemaphoreTake(sLock, portMAX_DELAY);
  AutoInterface::Peer* slot = nullptr;
  for (auto& p : sPeers) if (p.addr[0] && strcmp(p.addr, addr) == 0) { slot = &p; break; }
  uint32_t evicted = 0;
  bool fresh = false;
  if (!slot && create) {
    for (auto& p : sPeers) if (!p.addr[0]) { slot = &p; break; }
    if (!slot) {
      // Full: give the slot to the newcomer by taking the one nobody has
      // heard from for longest. Refusing instead meant the table froze
      // around whichever peers happened to be found first, and said nothing.
      AutoInterface::Peer* oldest = &sPeers[0];
      for (auto& p : sPeers) if ((int32_t)(p.lastSeenMs - oldest->lastSeenMs) < 0) oldest = &p;
      log_w("AutoInterface: peer table full (%d), dropping %s for %s",
            AUTOIF_MAX_PEERS, oldest->addr, addr);
      evicted = oldest->id;
      slot = oldest;
    }
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->addr, addr, sizeof(slot->addr));
    slot->id = AutoInterface::AUTO_ID_BASE | sNextPeer++;
    // A build without IPv6 address scopes reports no scope on an incoming
    // datagram. There is then only one link a peer can plausibly be on, so
    // take the first joined one rather than leaving the peer unaddressable.
    for (const Link& l : sLinks) if (l.joined) { slot->ifindex = l.ifindex; break; }
    fresh = true;
  }
  if (!slot) { xSemaphoreGive(sLock); return 0; }
  // Always follow the netif a peer is actually heard on. Taking it only when
  // the peer was created, and defaulting to the access point when the caller
  // had none to give, scoped replies to the wrong link: a LAN peer we could
  // hear perfectly never heard a word back, until it timed out and was
  // rediscovered from a packet that happened to carry the right index.
  if (ifindex) slot->ifindex = ifindex;
  slot->lastSeenMs = millis();
  if (data) slot->datagrams++;
  uint32_t id = slot->id;
  xSemaphoreGive(sLock);
  if (evicted) RnsTransport::clientDisconnected(evicted);
  if (fresh) {
    log_i("AutoInterface: new peer %s (#%lu)", addr, (unsigned long)(id & 0xFFFF));
    RnsTransport::clientConnected(id, addr);
  }
  return id;
}

// A peering token creates or refreshes a peer; a data datagram only refreshes
// one that peering has already vouched for.
uint32_t notePeering(const char* addr, int ifindex) { return touchPeer(addr, true,  false, ifindex); }
uint32_t noteData   (const char* addr, int ifindex) { return touchPeer(addr, false, true,  ifindex); }

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

void fillAddress(sockaddr_in6& out, const char* addr, uint16_t port, int ifindex) {
  memset(&out, 0, sizeof(out));
  out.sin6_family = AF_INET6;
  out.sin6_port = htons(port);
  inet_pton(AF_INET6, addr, &out.sin6_addr);
#if LWIP_IPV6_SCOPES
  out.sin6_scope_id = ifindex;
#else
  (void)ifindex;
#endif
}

bool peerAddress(uint32_t id, sockaddr_in6& out) {
  bool ok = false;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0] && p.id == id) {
    fillAddress(out, p.addr, kDataPort, p.ifindex);
    ok = true; break;
  }
  xSemaphoreGive(sLock);
  return ok;
}

bool bindSocket(int& fd, uint16_t port, const char* what) {
  fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { log_e("AutoInterface: socket() for %s failed (errno %d)", what, errno); return false; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in6 any = {};
  any.sin6_family = AF_INET6; any.sin6_addr = in6addr_any; any.sin6_port = htons(port);
  if (bind(fd, (sockaddr*)&any, sizeof(any)) < 0) {
    log_e("AutoInterface: bind %u (%s) failed (errno %d)", port, what, errno);
    close(fd); fd = -1;
    return false;
  }
  // Non-blocking, because the receive loop waits on all three sockets at once
  // with select() and then drains each one until it is empty. It used to do a
  // blocking read of a single datagram per socket per pass, with a 200 ms
  // timeout on each: a busy data socket behind a quiet discovery socket was
  // served five packets a second while the rest overflowed the stack's buffer.
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return true;
}

bool openSockets() {
  if (!bindSocket(sDisc, kDiscPort, "discovery")) return false;
  if (!bindSocket(sUni,  kUniPort,  "reverse peering")) return false;
  if (!bindSocket(sData, kDataPort, "data")) return false;
  log_i("AutoInterface: listening on [%s]:%u, :%u (reverse) and :%u (data)",
        kGroupAddr, kDiscPort, kUniPort, kDataPort);
  return true;
}

void sendToken(int fd, const char* from, const char* to, uint16_t port, int ifindex, bool multicast) {
  if (!from[0]) return;
  uint8_t tok[32]; token(from, tok);
  sockaddr_in6 dst;
  fillAddress(dst, to, port, ifindex);
  if (multicast) setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
  if (sendto(fd, tok, sizeof(tok), 0, (sockaddr*)&dst, sizeof(dst)) < 0)
    log_w("AutoInterface: sendto %s failed (errno %d)", to, errno);
}

void sendDiscovery() {
  for (const Link& l : sLinks)
    if (l.joined) sendToken(sDisc, l.local, kGroupAddr, kDiscPort, l.ifindex, true);
}

// RNS also peers by unicast, to every peer it already knows, so that a link
// which drops link-local multicast — an access point that does not forward it
// between clients, a phone whose radio filters it while the screen is off —
// keeps its peerings alive instead of timing them out every 22 seconds. Only
// the multicast half was implemented here, which is why a node could sit on a
// LAN full of Reticulum and see none of it.
void sendReversePeering() {
  struct { char addr[46]; int ifindex; } targets[AUTOIF_MAX_PEERS];
  size_t n = 0;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0]) {
    strlcpy(targets[n].addr, p.addr, sizeof(targets[n].addr));
    targets[n].ifindex = p.ifindex;
    n++;
  }
  xSemaphoreGive(sLock);
  for (size_t i = 0; i < n; i++) {
    const Link* l = linkByIndex(targets[i].ifindex);
    if (!l) continue;
    sendToken(sUni, l->local, targets[i].addr, kUniPort, l->ifindex, false);
  }
}

int scopeOf(const sockaddr_in6& src) {
#if LWIP_IPV6_SCOPES
  return src.sin6_scope_id;
#else
  (void)src; return 0;
#endif
}

// A peering token, multicast to the group or unicast to us. Either proves the
// sender knows the group id, and either is enough to keep a peer alive.
void handleDiscovery(const uint8_t* buf, int n, const sockaddr_in6& src, const char* how) {
  char addr[46]; inet_ntop(AF_INET6, &src.sin6_addr, addr, sizeof(addr)); lowercase(addr);
  if (isOurAddress(addr)) return;                    // our own multicast, echoed back
  uint8_t expect[32]; token(addr, expect);
  if (n == 32 && memcmp(expect, buf, 32) == 0) notePeering(addr, scopeOf(src));
  else log_w("AutoInterface: %s datagram (%d bytes) from %s with a non-matching token", how, n, addr);
}

void handleData(const uint8_t* buf, int n, const sockaddr_in6& src) {
  char addr[46]; inet_ntop(AF_INET6, &src.sin6_addr, addr, sizeof(addr)); lowercase(addr);
  if (n > RNS_MTU) {
    log_w("AutoInterface: %d-byte datagram from %s is over the %d-byte MTU, dropped", n, addr, RNS_MTU);
    return;
  }
  uint32_t id = noteData(addr, scopeOf(src));        // peered senders only
  if (!id) return;
  // Same ring and layout as the TCP clients: [client id | packet]
  uint8_t item[sizeof(RnsTransport::TcpItemHeader) + RNS_MTU];
  RnsTransport::TcpItemHeader h{ id };
  memcpy(item, &h, sizeof(h));
  memcpy(item + sizeof(h), buf, n);
  if (xRingbufferSend(sInRing, item, sizeof(h) + n, pdMS_TO_TICKS(20)) != pdTRUE)
    log_w("AutoInterface: inbound ring full, dropping %d bytes", n);
}

// Empty one socket. The sockets are non-blocking, so this returns as soon as
// there is nothing left rather than after a single datagram. The batch cap is
// what keeps a flood from holding this task on core 0 indefinitely: hitting it
// leaves the rest queued, the caller yields, and the next pass picks them up.
const int kDrainBatch = 64;

template <typename Handler>
bool drain(int fd, uint8_t* buf, size_t cap, Handler handler) {
  for (int i = 0; i < kDrainBatch; i++) {
    sockaddr_in6 src; socklen_t sl = sizeof(src);
    int n = recvfrom(fd, buf, cap, 0, (sockaddr*)&src, &sl);
    if (n < 0) return false;                         // nothing left
    if (n > 0) handler(buf, n, src);                 // an empty datagram is not one of ours
  }
  return true;                                       // more waiting
}

void task(void*) {
  deriveGroupAddress();
  if (!openSockets()) { vTaskDelete(nullptr); return; }
  // Peering can start on whichever link comes up first and pick the other one
  // up later, so there is nothing to wait for beyond the sockets.
  refreshLinks();
  sEnabled = true;

  uint32_t lastAnnounce = 0, lastReverse = 0, lastSecond = 0;
  uint8_t buf[RNS_MTU + 64];
  for (;;) {
    uint32_t now = millis();
    if (now - lastAnnounce >= kAnnounceMs) { lastAnnounce = now; sendDiscovery(); }
    if (now - lastReverse  >= kReverseMs)  { lastReverse  = now; sendReversePeering(); }
    if (now - lastSecond   >= 1000)        { lastSecond   = now; expirePeers(); refreshLinks(); }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sDisc, &rd); FD_SET(sUni, &rd); FD_SET(sData, &rd);
    int maxfd = sDisc;
    if (sUni  > maxfd) maxfd = sUni;
    if (sData > maxfd) maxfd = sData;
    timeval tv = { 0, (int)(kSelectMs * 1000) };
    if (select(maxfd + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;

    bool more = false;
    if (FD_ISSET(sDisc, &rd))
      more |= drain(sDisc, buf, sizeof(buf), [](const uint8_t* b, int n, const sockaddr_in6& s) {
        handleDiscovery(b, n, s, "discovery"); });
    if (FD_ISSET(sUni, &rd))
      more |= drain(sUni, buf, sizeof(buf), [](const uint8_t* b, int n, const sockaddr_in6& s) {
        handleDiscovery(b, n, s, "reverse peering"); });
    if (FD_ISSET(sData, &rd))
      more |= drain(sData, buf, sizeof(buf), [](const uint8_t* b, int n, const sockaddr_in6& s) {
        handleData(b, n, s); });
    if (more) vTaskDelay(1);                         // let core 0 breathe under a flood
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
  // The multicast group lives on the Wi-Fi netifs, which do not exist with
  // Wi-Fi off. The lock above is created regardless, because the readers
  // below run whether or not peering does.
  if (!settings.links().wifiEnabled) { log_i("AutoInterface: Wi-Fi is off, nothing to peer on"); return; }
  // 8 KB: the reverse-peering pass copies the peer table onto the stack so it
  // can send without holding the lock, and that table is three times the size
  // it was.
  xTaskCreatePinnedToCore(task, "autoif", 8192, nullptr, 2, nullptr, 0);
}

bool enabled() { return sEnabled; }

size_t peerCount() {
  size_t k = 0;
  if (!sLock) return 0;
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
  if (!sLock) return 0;
  size_t k = 0;
  xSemaphoreTake(sLock, portMAX_DELAY);
  for (auto& p : sPeers) if (p.addr[0] && k < max) out[k++] = p;
  xSemaphoreGive(sLock);
  return k;
}

const char* localAddress() {
  for (const Link& l : sLinks) if (l.joined) return l.local;
  return "";
}

} // namespace AutoInterface
