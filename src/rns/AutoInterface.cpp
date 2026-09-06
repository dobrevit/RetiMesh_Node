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
#include <atomic>
#include <freertos/semphr.h>
#include <freertos/ringbuf.h>
#include "AutoIfPolicy.h"
#include "RnsAnnounce.h"
#include "RnsTransport.h"
#include "Settings.h"
#include "WifiManager.h"
#include "Diag.h"
#include "Lock.h"
#include "Watchdog.h"

namespace {

char kGroupId[33]        = AUTOIF_GROUP_ID;
char kGroupAddr[48]      = "";                 // derived from the group id at start
const uint16_t kDiscPort = 29716;
const uint16_t kUniPort  = 29716 + 1;          // RNS: discovery_port + 1
const uint16_t kDataPort = 42671;
// The discovery cadence lives in AutoIfPolicy.h: 1.6 s while anyone can hear
// it, backed off when nobody can. Reverse peering needs no such policy — it
// unicasts to known peers only, so with an empty table it sends nothing.
const uint32_t kReverseMs  = 5200;             // RNS: announce_interval * 3.25
const uint32_t kPeerTimeoutMs = 22000;
const uint32_t kSelectMs = 200;

int  sDisc = -1, sUni = -1, sData = -1;
bool sEnabled = false;
std::atomic<bool> sStop{false};                // end() asks; the task obliges
// The task stopped itself while still wanted — rebuildDiscovery lost the
// discovery socket and could not rebind. A self-stop happens with the
// radio's shape already settled, so no convergence would ever run again on
// its own; WifiManager's tick reads this (stoppedUnexpectedly()) to raise
// one, and the convergence's need-based restart re-begins the task. Cleared
// when begin() actually starts one.
std::atomic<bool> sSelfStopped{false};
std::atomic<bool> sTaskAlive{false};           // the task clears this as it goes
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
  // The membership was already given back while the netif lived (linkDown,
  // called by the Wi-Fi convergence before it strips the interface). The
  // link stays marked joined until the netif actually goes — the definition
  // of linkDown says why — and this is what tells the stale branch below
  // that there is nothing left to leave.
  bool        leftEarly = false;
};
Link sLinks[] = {
  { "WIFI_AP_DEF",  "the access point" },
  { "WIFI_STA_DEF", "the station network" },
};

bool bindSocket(int& fd, uint16_t port, const char* what);   // defined below

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

// A leave against an already-removed netif has stranded a membership slot on
// sDisc: lwIP's IPV6_LEAVE_GROUP handler resolves the netif index first and
// returns ENXIO *before* lwip_socket_unregister_mld6_membership runs, so the
// slot — one of CONFIG_LWIP_MAX_SOCKETS (16, the pinned sdkconfig) — stays
// occupied and no later setsockopt can free it; enough of them and every
// join is refused, once a second, for ever. The one thing that does free
// them is closing the socket (lwIP drops every registration a socket holds
// on close), so the discovery socket is rebuilt and every link marked
// unjoined; the normal join path re-joins the live ones within a second.
void rebuildDiscovery() {
  Sys::Lock held(sLock);         // clear-and-close under the lock (closeSockets' rule):
  int d = sDisc;                 //   linkDown snapshots this fd under it too
  sDisc = -1;
  if (d >= 0) close(d);
  for (Link& l : sLinks) { l.joined = false; l.ifindex = 0; l.local[0] = '\0'; l.leftEarly = false; }
  // The rebind publishes into sDisc under the same hold. bindSocket writes
  // the fd more than once on its way (the fresh socket, then closed-and--1
  // if the bind fails), and linkDown on the loop task snapshots sDisc under
  // its own hold — released here mid-rebuild, it could judge one of those
  // transients open and leave against an fd this call is about to close.
  // Safe to hold across: socket/setsockopt/bind/fcntl each take lwIP's core
  // lock briefly, the same sLock -> lwIP order linkDown documents.
  const bool bound = bindSocket(sDisc, kDiscPort, "discovery");
  held.release();
  if (!bound) {
    // No socket to select on. Ask the task to end through its own stop path
    // (endTask: peers disconnected, remaining sockets closed, watchdog
    // unsubscribed). Nothing brackets this stop the way WifiManager's own
    // end() call does, so the self-stop is flagged: WifiManager's tick reads
    // stoppedUnexpectedly(), raises a convergence pass for it, and that
    // pass's need-based restart re-begins.
    log_e("AutoInterface: could not rebuild the discovery socket; stopping");
    sSelfStopped = true;
    sStop = true;
    return;
  }
  log_w("AutoInterface: discovery socket rebuilt to free a stranded group membership");
}

// Join the discovery group on any link that has come up and is not joined
// yet. Called at start and once a second afterwards, so a station that
// associates minutes after boot is peered on without a restart.
void refreshLinks() {
  for (Link& l : sLinks) {
    if (l.joined) {
      // Still the interface we joined? A netif cycle while the task keeps
      // running — the runtime AP down/up path takes the AP netif away and
      // brings back a fresh one, with a fresh ifindex — would otherwise
      // leave the link marked joined against an interface that no longer
      // exists, and this loop only ever joins the not-joined: discovery on
      // that link would be dead for the life of the task with nothing in
      // the log to say so. Judged by the ifindex, the link-local's presence
      // AND its text, because any of the three goes first depending on how
      // the netif went down. The ifindex term carries the normal cycle:
      // esp_netif's stop removes the lwIP netif and its start adds a fresh
      // one, and lwIP's netif_add hands out netif->num from a rolling
      // counter over 0..254 that skips numbers in use (netif.c) — so a
      // cycled netif cannot see its old index again until 255 allocations
      // have gone by, each of them a whole link cycle somewhere on this
      // node, and this pass runs every second: the absent netif is seen
      // long before any wrap could land the old number back. The address
      // term is the belt for an index that does come back reused with a
      // different link-local on it — then the text differs and the link is
      // still retired and re-joined rather than left deaf.
      // A link linkDown() has already left holds this branch too: while the
      // netif it anticipated losing still stands, the continue below leaves
      // the link exactly as it is — unjoining it early would let the next
      // pass re-join a netif that is about to die, recreating the stranded
      // slot linkDown exists to prevent.
      esp_netif_t* netif = esp_netif_get_handle_from_ifkey(l.key);
      const int nowIndex = netif ? esp_netif_get_netif_impl_index(netif) : 0;
      char nowLocal[sizeof(l.local)];
      if (nowIndex == l.ifindex && linkLocalOf(l.key, nowLocal, sizeof(nowLocal)) &&
          strcmp(nowLocal, l.local) == 0) continue;
      bool leftEarly;
      { Sys::Lock held(sLock); leftEarly = l.leftEarly; }
      bool stranded = false;
      if (!leftEarly) {
        // Give the membership back against the old ifindex, best effort: if
        // lwIP kept it across a stop/start it would refuse the fresh join
        // below. Not attempted when linkDown already left while the netif
        // lived — there is nothing registered, and the ENXIO this would
        // return is indistinguishable from the stranding checked for next.
        ipv6_mreq old = {};
        inet_pton(AF_INET6, kGroupAddr, &old.ipv6mr_multiaddr);
        old.ipv6mr_interface = l.ifindex;
        if (setsockopt(sDisc, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &old, sizeof(old)) < 0 &&
            errno == ENXIO) {
          // The netif went before the leave could reach it, and lwIP's
          // ENXIO path returns before the socket's membership slot is
          // unregistered — that slot (one of CONFIG_LWIP_MAX_SOCKETS = 16)
          // is now stranded on sDisc, and each one lost is a join the
          // future is refused. Only a socket rebuild frees it. Any other
          // failure (EADDRNOTAVAIL: the netif stands, the group was not
          // on it) has still unregistered the slot and needs nothing.
          stranded = true;
        }
      }
      Sys::Lock held(sLock);
      l.joined = false; l.ifindex = 0; l.local[0] = '\0'; l.leftEarly = false;
      held.release();
      log_i("AutoInterface: %s went away; peering resumes when it returns", l.what);
      if (stranded) rebuildDiscovery();
      continue;   // the netif is down or mid-change; the join below waits its turn
    }
    char local[sizeof(l.local)];
    if (!linkLocalOf(l.key, local, sizeof(local))) continue;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey(l.key);
    int ifindex = netif ? esp_netif_get_netif_impl_index(netif) : 0;
    ipv6_mreq mreq = {};
    inet_pton(AF_INET6, kGroupAddr, &mreq.ipv6mr_multiaddr);
    mreq.ipv6mr_interface = ifindex;
    if (setsockopt(sDisc, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) < 0) {
      log_w("AutoInterface: could not join the group on %s (errno %d)", l.what, errno);
      continue;
    }
    // Committed under the lock: localAddress() reads a link from other tasks,
    // and joined must not read true before the address next to it is whole.
    Sys::Lock held(sLock);
    strlcpy(l.local, local, sizeof(l.local));
    l.ifindex = ifindex;
    l.joined  = true;
    l.leftEarly = false;
    held.release();
    log_i("AutoInterface: peering on %s, link-local %s (ifindex %d)", l.what, l.local, l.ifindex);
  }
}

// Returns the peer id, registering a new peer with Transport. Only a peering
// token creates one: a data datagram from an address that has not proved it
// knows the group id is not evidence of a peer, and RNS reads it the same way
// (AutoInterface.process_incoming ignores senders it has not peered with).
uint32_t touchPeer(const char* addr, bool create, bool data, int ifindex) {
  Sys::Lock held(sLock);
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
  if (!slot) return 0;
  // Always follow the netif a peer is actually heard on. Taking it only when
  // the peer was created, and defaulting to the access point when the caller
  // had none to give, scoped replies to the wrong link: a LAN peer we could
  // hear perfectly never heard a word back, until it timed out and was
  // rediscovered from a packet that happened to carry the right index.
  if (ifindex) slot->ifindex = ifindex;
  slot->lastSeenMs = millis();
  if (data) slot->datagrams++;
  uint32_t id = slot->id;
  held.release();                         // the calls below must not hold it
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
  Sys::Lock held(sLock);
  for (auto& p : sPeers)
    if (p.addr[0] && millis() - p.lastSeenMs > kPeerTimeoutMs) {
      log_i("AutoInterface: peer %s timed out", p.addr);
      expired[n++] = p.id; p.addr[0] = '\0';
    }
  held.release();
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

// Close whichever of the three sockets are open — under sLock, clear and
// close together. The variables reading "closed" before the fds went was
// necessary and not sufficient: sendTo on the RNS task could snapshot a
// live fd, lose the CPU, and issue its sendto after the close — into a
// number lwIP may already have handed to some future socket. It now holds
// the same lock across its snapshot-and-send (and linkDown across its
// snapshot-and-leave), so an fd judged open under the lock stays that fd
// until the holder is done with it. The closes are quick (UDP, nothing to
// linger on), so nobody waits behind them long.
void closeSockets() {
  Sys::Lock held(sLock);
  int d = sDisc, u = sUni, dat = sData;
  sDisc = sUni = sData = -1;
  if (d   >= 0) close(d);                 // closing sDisc drops the group memberships
  if (u   >= 0) close(u);
  if (dat >= 0) close(dat);
}

bool openSockets() {
  if (!bindSocket(sDisc, kDiscPort, "discovery") ||
      !bindSocket(sUni,  kUniPort,  "reverse peering") ||
      !bindSocket(sData, kDataPort, "data")) {
    // All or nothing: a socket bound before the failure must not outlive it.
    // Left open, each failed begin()/end() cycle leaked up to two lwIP
    // socket slots — from a table small enough that the web server starves.
    closeSockets();
    return false;
  }
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
  Sys::Lock held(sLock);
  for (auto& p : sPeers) if (p.addr[0]) {
    strlcpy(targets[n].addr, p.addr, sizeof(targets[n].addr));
    targets[n].ifindex = p.ifindex;
    n++;
  }
  held.release();                         // sending must not hold it
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
    // end() may have asked mid-drain. Checked per datagram because each one
    // can legitimately cost 20 ms (the ring-full timeout in handleData), so
    // a full pass over three sockets under a flood approaches two seconds —
    // and a flood is exactly when the Wi-Fi teardown calls end(). Bailing
    // here puts the task back at the top of its loop, where endTask() runs.
    if (sStop) return false;
    sockaddr_in6 src; socklen_t sl = sizeof(src);
    int n = recvfrom(fd, buf, cap, 0, (sockaddr*)&src, &sl);
    if (n < 0) return false;                         // nothing left
    if (n > 0) handler(buf, n, src);                 // an empty datagram is not one of ours
  }
  return true;                                       // more waiting
}

// The task's own exit, on end()'s request. Runs on the task so that nothing
// here ever races the select loop: by the time end() returns, this has
// finished. Transport is told first — with the table empty, sendTo() on the
// RNS task resolves no peer and so never reaches a socket being closed below.
void endTask() {
  uint32_t live[AUTOIF_MAX_PEERS]; size_t n = 0;
  Sys::Lock held(sLock);
  for (auto& p : sPeers) if (p.addr[0]) { live[n++] = p.id; p.addr[0] = '\0'; }
  held.release();                         // the calls below must not hold it
  for (size_t i = 0; i < n; i++) RnsTransport::clientDisconnected(live[i]);
  sEnabled = false;
  closeSockets();
  // A netif cycle — which is what end() exists for — hands out fresh
  // ifindexes and link-locals, so everything remembered about the links is
  // stale. Dropped here so a later begin() joins from nothing; under the
  // lock, because localAddress() reads the links from other tasks.
  Sys::Lock links(sLock);
  for (Link& l : sLinks) { l.joined = false; l.ifindex = 0; l.local[0] = '\0'; l.leftEarly = false; }
  links.release();
  log_i("AutoInterface: stopped; %u peer%s disconnected", (unsigned)n, n == 1 ? "" : "s");
  // Off the watchdog before the task goes: nothing in IDF clears a
  // subscription when its task is deleted, and an entry that can never be
  // fed again panics the node one timeout later (Watchdog.h).
  Watchdog::unwatch();
  sTaskAlive = false;                     // last: end() is waiting on it
  vTaskDelete(nullptr);
}

void task(void*) {
  deriveGroupAddress();
  if (!openSockets()) { sTaskAlive = false; vTaskDelete(nullptr); return; }
  // Peering can start on whichever link comes up first and pick the other one
  // up later, so there is nothing to wait for beyond the sockets.
  refreshLinks();
  sEnabled = true;
  Watchdog::watch();

  uint32_t lastReverse = 0, lastSecond = 0;
  AutoIfPolicy policy;
  // Presence as of the last one-second slot below. Reading it every pass
  // would buy nothing but lock and driver traffic; a second of staleness is
  // well inside "peers within seconds". The zeros before the first slot cost
  // nothing either: the policy's first ask sends regardless.
  bool apStation = false, staAssociated = false;
  size_t livePeers = 0;
  uint8_t buf[RNS_MTU + 64];
  // Guarded whole rather than per statement: the loop below uses continue,
  // which cannot cross a lambda. A throw leaves the inner loop, is
  // reported, and the outer one goes back in (Diag.h).
  for (;;) {
    Diag::guard("the autointerface task", [&] {
      for (;;) {
        // The inner loop is the one that spins; a throw leaves it and the outer
        // one goes back in, so this is where progress is actually reported.
        Watchdog::feed();
        if (sStop) endTask();             // does not return
        uint32_t now = millis();
        if (now - lastSecond >= 1000) {
          lastSecond = now; expirePeers(); refreshLinks();
          apStation     = WiFi.softAPgetStationNum() > 0;
          staAssociated = wifiManager.stationConnected();
          livePeers     = AutoInterface::peerCount();
        }
        // refreshLinks can lose its socket and ask to stop (rebuildDiscovery
        // with no fd to rebind into): back to the top, where endTask runs,
        // rather than on to an FD_SET of -1.
        if (sStop) continue;
        if (policy.discoveryDue(now, apStation, staAssociated, livePeers)) sendDiscovery();
        if (now - lastReverse >= kReverseMs) { lastReverse = now; sendReversePeering(); }

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
    });
    // Something threw and was contained. A short wait, then back in:
    // retrying at full speed into the same wall helps nobody.
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

} // namespace

namespace AutoInterface {

bool wanted() {
  return HAS_AUTOINTERFACE && settings.transport().autoEnabled;
}

void begin(RingbufHandle_t inRing) {
  if (sTaskAlive) {                              // already peering; nothing to redo —
    if (sStop)                                   // unless an end() timed out on this task:
      log_w("AutoInterface: not restarted — the previous task never answered end()");
    return;                                      // then the node runs on without peering,
  }                                              // and this line is the only trace of why
  // The lock is created once and lives for ever — the heartbeat and the API
  // read the peer table through it whether or not peering runs, and end()
  // deliberately leaves it standing for them.
  if (!sLock) sLock = xSemaphoreCreateMutex();
  sInRing = inRing;
  Sys::Lock held(sLock);
  memset(sPeers, 0, sizeof(sPeers));
  held.release();
  strlcpy(kGroupId, settings.transport().autoGroupId[0] ? settings.transport().autoGroupId : AUTOIF_GROUP_ID, sizeof(kGroupId));
  if (!wanted()) { log_i("AutoInterface disabled in settings"); return; }
  // The multicast group lives on the Wi-Fi netifs, which do not exist with
  // Wi-Fi off. The lock above is created regardless, because the readers
  // below run whether or not peering does.
  if (!settings.links().wifiEnabled()) { log_i("AutoInterface: Wi-Fi is off, nothing to peer on"); return; }
  sStop = false;
  sSelfStopped = false;                          // this start answers the self-stop
  sTaskAlive = true;                             // before the task can run and clear it
  // 8 KB: the reverse-peering pass copies the peer table onto the stack so it
  // can send without holding the lock, and that table is three times the size
  // it was.
  if (!Diag::startTask(task, "autoif", 8192, nullptr, 2, 0)) sTaskAlive = false;
}

// The Wi-Fi re-up transition's begin: the ring cannot change after boot, so
// the one the boot begin() stored serves every later start. Without a boot
// begin() there is nothing to restart and nowhere for datagrams to go.
void begin() {
  if (!sInRing) { log_w("AutoInterface: never begun with a ring; nothing to restart"); return; }
  begin(sInRing);
}

bool end() {
  // The caller is the Wi-Fi teardown (WifiManager::syncRadioShape), which
  // must not cycle the netifs while the discovery group is still joined on
  // them. Kept synchronous for exactly that caller: when this returns true,
  // the task has disconnected its peers, closed its sockets and gone.
  if (!sTaskAlive) return true;
  sStop = true;
  // The task notices within one select() pass — the drains check sStop per
  // datagram, so a flood cannot hold a pass for its full two-second worst
  // case — and confirms by clearing the aliveness flag on its way out
  // (endTask). Bounded, so a wedged task hangs the watchdog's timeout rather
  // than this caller; 5 s clears the legitimate worst case (a 200 ms select,
  // one last ring-full datagram, endTask's own disconnect posts) with margin,
  // and sits well inside the caller's own 30 s watchdog (WATCHDOG_TIMEOUT_S).
  // False tells that caller the netifs are not safe to cycle yet; it retries
  // the pass and bounds how many times (the header says why).
  for (int i = 0; i < 500 && sTaskAlive; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if (sTaskAlive) { log_w("AutoInterface: the task did not stop inside 5 s"); return false; }
  return true;
}

// The Wi-Fi convergence is about to strip one netif while the task keeps
// running. The leave must happen now, against the live netif: lwIP's
// IPV6_LEAVE_GROUP resolves the netif index first and returns ENXIO *before*
// lwip_socket_unregister_mld6_membership runs, so a leave after the netif is
// gone strands the socket's membership slot — one of CONFIG_LWIP_MAX_SOCKETS
// (16). Each runtime AP down/up cycle then leaked one, and once the table
// filled every join was refused: peering fell silent with a once-a-second
// log line as the only symptom. (The whole-radio-off path never needed this:
// end() closes the socket, and closing drops all of its registrations.)
//
// Runs on the caller's task (the loop task) while the autoif task keeps
// selecting on the same socket, and that is safe: lwIP serialises the two
// through the stack's core lock (CONFIG_LWIP_TCPIP_CORE_LOCKING=1 in the
// pinned sdkconfig — this setsockopt runs on the calling task under that
// lock, not as a message to the tcpip thread), the fd is never closed here,
// and the only thing that changes is a group membership — at worst the
// select loop stops seeing that group's datagrams, which is the point. What
// would NOT be safe is marking the link unjoined here: refreshLinks runs
// once a second on the autoif task, and an unjoined link whose netif is
// still alive — which it is until WiFi.mode() lands, some time after this
// returns — is exactly what refreshLinks re-joins, re-registering the
// membership this call just gave back on a netif about to die. So only
// leftEarly is set (under sLock, like every sLinks write since M2), the
// link stays marked joined, and the autoif task itself retires the mark on
// its next pass after the netif has actually changed — knowing from
// leftEarly that there is nothing to leave.
void linkDown(const char* netifKey) {
  if (!sLock) return;
  Link* target = nullptr;
  for (Link& l : sLinks) if (strcmp(l.key, netifKey) == 0) { target = &l; break; }
  if (!target) return;
  // The fd snapshot, the judgement and the setsockopt under the one lock
  // (closeSockets' rule, same as sendTo): the autoif task closes sDisc under
  // it — endTask, or a rebuild — so the fd judged open here cannot go, and
  // its number be reassigned, before the leave has been issued. The leave
  // runs on this (the loop) task under lwIP's core lock and is quick; the
  // lock order is only ever sLock -> lwIP, never the reverse, so nothing
  // can deadlock on it.
  Sys::Lock held(sLock);
  const int fd = sDisc;
  if (fd < 0) return;
  if (!target->joined || target->leftEarly) return;  // never joined, or already given back
  ipv6_mreq mreq = {};
  inet_pton(AF_INET6, kGroupAddr, &mreq.ipv6mr_multiaddr);
  mreq.ipv6mr_interface = target->ifindex;
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq)) < 0) {
    // Nothing more to do here: refreshLinks' stale branch attempts the same
    // leave once the netif is gone, and its safety net rebuilds the socket
    // if that strands the slot.
    log_w("AutoInterface: could not leave the group before %s goes (errno %d)", netifKey, errno);
    return;
  }
  target->leftEarly = true;
}

bool enabled() { return sEnabled; }

// Both terms, not the flag alone: between rebuildDiscovery raising the flag
// and endTask clearing the aliveness — normally one select() pass, but for
// ever if the task wedges on its way out — the flag alone had WifiManager's
// tick raise a convergence on every pass, and each one could only log that
// begin() cannot restart over a living task (its sTaskAlive guard). Requiring
// the task actually gone makes the signal fire exactly when a restart can
// act on it. sSelfStopped itself is NOT cleared while the task lives — that
// would lose the restart when the task does finally go.
bool stoppedUnexpectedly() { return sSelfStopped && !sTaskAlive; }

size_t peerCount() {
  size_t k = 0;
  if (!sLock) return 0;
  Sys::Lock held(sLock);
  for (auto& p : sPeers) if (p.addr[0]) k++;
  return k;
}

bool sendTo(uint32_t peerId, const uint8_t* packet, size_t len) {
  // The fd check, the peer lookup and the sendto under the one lock:
  // closeSockets clears and closes under it too, so the fd this judged open
  // cannot be closed — and its number reassigned by lwIP to a future socket
  // — between the check and the send. Runs on the RNS task; the socket is
  // non-blocking UDP, so the send is a copy into a pbuf or an immediate
  // refusal, and the lock is held for microseconds — nothing the autoif
  // task's once-a-second bookkeeping or the status readers notice.
  if (!sLock) return false;
  Sys::Lock held(sLock);
  if (sData < 0) return false;
  for (auto& p : sPeers) {
    if (!p.addr[0] || p.id != peerId) continue;
    sockaddr_in6 dst;
    fillAddress(dst, p.addr, kDataPort, p.ifindex);
    int n = sendto(sData, packet, len, 0, (sockaddr*)&dst, sizeof(dst));
    if (n < 0) { log_w("AutoInterface: sendto failed (errno %d)", errno); return false; }
    return true;
  }
  return false;                                    // no such peer any more
}

size_t peers(Peer* out, size_t max) {
  if (!sLock) return 0;
  size_t k = 0;
  Sys::Lock held(sLock);
  for (auto& p : sPeers) if (p.addr[0] && k < max) out[k++] = p;
  return k;
}

const char* localAddress() {
  // Copied out under the lock: the task rewrites the links as they come and
  // go (refreshLinks, endTask), and this is read from other tasks. One call
  // site (the Wi-Fi status JSON) reads the returned buffer.
  if (!sLock) return "";
  static char out[46];
  Sys::Lock held(sLock);
  out[0] = '\0';
  for (const Link& l : sLinks) if (l.joined) { strlcpy(out, l.local, sizeof(out)); break; }
  return out;
}

} // namespace AutoInterface
