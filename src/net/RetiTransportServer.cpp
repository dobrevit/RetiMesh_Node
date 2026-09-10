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
#include <new>                           // std::nothrow

#include "RetiTransportServer.h"
#include "RnsTransport.h"
#include "Bootloader.h"
#include "ClientAdmit.h"
#include "Diag.h"
#include "Lock.h"

RetiTransportServer transportServer;

// ---------------------------------------------------------------------------
void RetiTransportServer::begin(RingbufHandle_t tcpInRing) {
  _tcpInRing = tcpInRing;
  _lock = xSemaphoreCreateMutex();

  // Room for every client this node can ever hold, taken once. The cap is
  // fixed at compile time (RNS_MAX_CLIENTS, Config.h) and onClient() refuses a
  // peer past it before it allocates a ClientCtx or pushes one, while the
  // erase() on disconnect never shrinks capacity — so with the slots reserved
  // here, push_back has no reason to allocate again for the life of the run.
  // That is the point of it, not a micro-optimisation: push_back runs on the
  // AsyncTCP event task, and an allocation that does not happen is the only one
  // that cannot throw there.
  //
  // Guarded like the snapshot buffers in RnsTransport::begin(): memory asked
  // for this early can fail on the tight boards, and failing has to cost the
  // reserve rather than the boot — a throw out of here would reach setup()
  // with nothing to catch it. The ask is one pointer per client slot, which is
  // why it is worth making at all.
  //
  // And when it does fail the list grows on demand again, exactly as it did
  // before: push_back can still allocate, and can still throw. Whatever
  // handles that stays needed — this reserve makes the path rare, not
  // unreachable. Both halves of that are read off a real vector in
  // test/test_client_admit, the reserved one and the one whose reserve threw.
  Diag::guard("reserving the client slots", [this] { _clients.reserve(RNS_MAX_CLIENTS); });

  // Checked, for the same reason the client contexts below are: this is memory
  // asked for on a board that can be out of it, and the line under it used to
  // dereference the answer without looking. A null costs the listener rather
  // than the boot — the node still routes over LoRa, still serves its portal
  // and still answers on the console — so it says which port nobody will be
  // able to reach and leaves everything else running.
  _server = new (std::nothrow) AsyncServer(RNS_TCP_PORT);
  if (_server == nullptr) {
    log_e("Reticulum transport not listening on port %d: the listener could not "
          "be allocated. Clients will not be able to connect over TCP", RNS_TCP_PORT);
    return;
  }

  _server->setNoDelay(true);
  _server->onClient([](void* self, AsyncClient* client) {
    static_cast<RetiTransportServer*>(self)->onClient(client);
  }, this);
  _server->begin();

  log_i("Reticulum transport listening on 0.0.0.0:%d", RNS_TCP_PORT);
}

size_t RetiTransportServer::clientCount() {
  { Sys::Lock held(_lock);
    return _clients.size();
  }
}

// ---------------------------------------------------------------------------
// Naming a peer, on a task and at the moment the heap may be gone.
// ---------------------------------------------------------------------------
// Two places below put a peer's address in a log line, and both run on the
// AsyncTCP event task: onClient() while a starved node is refusing clients, and
// onData() while a peer is framing packets past the MTU. Neither may allocate
// to do it, and the obvious way does.
//
// remoteIP().toString() builds an Arduino String, which reaches realloc past
// its small-string buffer (WString.cpp:212). It does not throw when that fails
// — String::copy() calls invalidate() (WString.cpp:236-237), invalidate() calls
// init() (:165-170), and init() leaves a non-SSO String with a null buffer
// (:158-163). c_str() is buffer() (WString.h:258-259), and buffer() on a
// non-SSO String returns ptr.buff (:381-383): a null pointer, not "". These
// builds link the full formatter — CONFIG_NEWLIB_NANO_FORMAT is off in the
// prebuilt IDF (framework-arduinoespressif32-libs/esp32s3/sdkconfig:4628,
// esp32/sdkconfig:3745) — and no member of newlib's vfprintf family puts
// anything in the place of a null "%s". The one log_w reaches is _svfprintf_r:
// log_w is log_printf (esp32-hal-log.h:211), which is log_printfv, which
// formats with vsnprintf (esp32-hal-uart.c:2105-2112, :2063-2085), and
// vsnprintf is a wrapper on _svfprintf_r. Its whole string pool, and
// _vfprintf_r's beside it, is INF, inf, NAN, nan, the two digit tables and "0"
// — no "(null)" in either, on either target. So "%s" on that pointer loads
// through zero: a LoadProhibited panic, on the one path in this file that
// exists so a node out of memory refuses a client instead of panicking.
//
// So the address is printed rather than built, and what makes reading it safe
// where a refusal is possible is that it is allocation-free — not that nothing
// throws. IPAddress::printTo() is the core's own formatter and writes to any
// Print (IPAddress.cpp:306-387), a character or a number at a time, and Print
// converts numbers in a stack buffer (Print.cpp:252-271). Nothing on that path
// allocates. Going through the core's printer rather than the octets also means
// the IPv6 canonical form is not written out a second time here, and the text
// is byte for byte the text toString() produced.
//
// IPv6 is not hypothetical on this listener. The prebuilt IDF sets
// CONFIG_LWIP_IPV6=y (esp32s3/sdkconfig:2634, esp32/sdkconfig:2129), the
// AsyncServer(port) constructor begin() uses binds IPADDR_TYPE_ANY under that
// (AsyncTCP 3.5.0, AsyncTCP.cpp:1497-1504), and remoteIP() reports whatever
// family the pcb holds (:1353-1364). printTo() renders both, so both are named
// in full instead of one of them being named wrongly.
namespace {

// A Print that fills a fixed buffer and keeps it terminated, dropping whatever
// will not fit. No allocation, no ownership, and truncation is its only failure
// mode — which PEER_NAME_MAX below exists to make unreachable.
//
// write() answers bytes *consumed*, not bytes stored, so a byte it dropped
// still counts 1. Deliberate, and safe here because nothing on this path reads
// the answer: Print::write(buffer, size) only sums it (Print.cpp:38-44) and
// printNumber() only returns that sum (:252-271). Answering 0 for a dropped
// byte would make those sums say a digit was never written, which is a short
// write to any caller that ever does look.
class FixedPrint : public Print {
public:
  FixedPrint(char* buf, size_t cap) : _buf(buf), _cap(cap) { if (_cap) _buf[0] = '\0'; }
  using Print::write;
  size_t write(uint8_t c) override {
    if (_n + 1 < _cap) { _buf[_n++] = (char)c; _buf[_n] = '\0'; }
    return 1;
  }

private:
  char*  _buf;
  size_t _cap;
  size_t _n = 0;
};

// What "address:port" is worth at its longest, derived from the printer that
// produces it rather than borrowed. printTo() renders IPv6 as eight fields of
// at most four hex digits with seven colons between them, never in the dotted
// IPv4-mapped form (IPAddress.cpp:309-378), and appends no zone because none is
// asked for (:365-376): 39 characters, against 15 for IPv4. Then a colon, a
// port of at most five digits, and the terminator — 39 + 1 + 5 + 1 = 46.
//
// It lands on the same 46 as INET6_ADDRSTRLEN by coincidence and not by
// derivation: that figure's worst case is the ::ffff:255.255.255.255 form this
// printer cannot emit.
//
// Worth doing properly because this buffer is what the name is built in, and
// because it is the only buffer on the way to Transport that is wide enough.
// It survives the next hop: RnsTransport's Event::remote is 46 as well
// (RnsTransport.cpp:447), so the strlcpy into it cannot truncate
// (RnsTransport.cpp:1800) and the port is still there afterwards.
//
// It does not survive the hop after that, and this comment used to claim it
// did. The name Transport hashes is not e.remote — it is built from it, into
// INTERFACE_NAME_MAX bytes, which is 32 (Config.h:389), as "WiFi/" or "Auto/"
// and then the remote (interfaceName(), RnsTransport.cpp:470, called at :1850).
// Five characters of prefix leave 26 for address and port. IPv4 is safe with
// room over: "255.255.255.255:65535" is 21. IPv6 is not, and this listener
// really does see it — the block above says why — because a link-local like
// fe80::1234:5678:9abc:def0:54321 is 31 characters and is cut at 26, which
// takes the port off entirely. Two sockets from one IPv6 host then hash to one
// name.
//
// So on IPv6 that collision is handled rather than avoided: evictCollision()
// (RnsTransport.cpp:1832-1843, called at :1855) finds the older interface
// already registered under the hash, drops it and says so, and the newer socket
// works. Widening the name is not this buffer's to do — INTERFACE_NAME_MAX also
// sizes PathInfo::via and IfaceInfo::name (RnsTransport.h:74-75), which
// /api/status (WifiManager.cpp:2256) and the display read.
constexpr size_t PEER_NAME_MAX = 46;

// "address:port", the way RNS's own TCPServerInterface names a spawned
// interface. Transport identifies an interface by the hash of that name, so the
// port is what keeps a phone that reconnects from the same address from
// colliding with the interface its previous socket still holds — for as far as
// the port survives, which the block above measures: all the way on IPv4, and
// not as far as the hash on IPv6.
void peerName(const AsyncClient* client, char* out, size_t cap) {
  FixedPrint p(out, cap);
  client->remoteIP().printTo(p, false);
  p.print(':');
  p.print((unsigned)client->remotePort());
}

} // namespace

// ---------------------------------------------------------------------------
// Connection lifecycle (all callbacks run on the AsyncTCP event task).
// ---------------------------------------------------------------------------
// The server hands over a client it allocated; one that is turned away has
// to be freed by whoever turned it away, once the close has gone through.
// Closing alone leaked it — both the limit branch and the restart branch
// did — one per connection attempt, for as long as the condition held.
//
// Note what that costs the caller: close() reaches AsyncClient::_close(), which
// runs the disconnect callback *inline* when the close succeeds (AsyncTCP
// 3.5.0, AsyncTCP.cpp:1016-1025), so the delete below usually happens before
// this function returns. `client` is spent from here — which is why onClient()
// reads the peer's address before it can be refused, and touches nothing of it
// afterwards. ClientAdmit.h states that as the contract refuse() has to meet.
static void refuse(AsyncClient* client) {
  client->onDisconnect([](void*, AsyncClient* c) { delete c; }, nullptr);
  client->close();
}

// One event in, one answer out: does this one also get a line? The two lines
// in this file that ask this are the refusals a starved node hands out, and
// one peer's stream of oversize frames — with their own period each and their
// own instance each.
//
// It is one rule because it was two, and the two did not agree. The refusals
// let the first event speak; the oversize line dropped it whenever it fell in
// the first period of an uptime, because its `lastMs` starts at zero and zero
// early in a boot is a window that has not opened yet rather than one that
// closed long ago. The first event is the one worth having: a single refusal,
// or a single oversize frame, on an otherwise healthy node is precisely the
// reading an operator is looking for. So `events` decides that, not the clock.
//
// Counting is unconditional and is never what gets rated. The lines report
// running totals, and a total that moved only when a line came out would make
// three hundred refusals read as one — the same argument Diag makes for the
// containment count it is handed (Diag.cpp:136-149), and the shape its own
// allocation line already had (Diag.cpp:176-183).
//
// No lock, and none needed: both callers are AsyncTCP callbacks — onClient()
// and onData() — and that framework runs them on its one service task, so no
// two calls into a given instance overlap.
bool RetiTransportServer::RateLimit::due(uint32_t periodMs) {
  events++;
  const uint32_t now = millis();
  if (events > 1 && now - lastMs < periodMs) return false;
  lastMs = now;
  return true;
}

void RetiTransportServer::onClient(AsyncClient* client) {
  if (client == nullptr) return;

  // Read before anything can turn this client away, because a refusal frees it
  // — refuse() above says on which line — so every line below that names the
  // peer names this buffer and never the socket.
  //
  // Once, for the refusals and the accept alike. Safe to take here, ahead of
  // the first answer that can refuse, because peerName() cannot allocate and so
  // has nothing to fail at: the block above it is the whole of why that
  // matters.
  char remote[PEER_NAME_MAX];
  peerName(client, remote, sizeof(remote));

  // Who gets a slot, and who frees what when nobody does: ClientAdmit.h owns
  // the order and the accounting, and everything that touches hardware is
  // passed to it. The context is published by enrol(), at its end, rather than
  // captured out of the allocation: enrol() only ever finishes on the accepted
  // path, whereas a pointer taken from the allocation would already have been
  // released by the time a failed enrolment came back — ClientAdmit.h states
  // that obligation, and this is the shape that meets it.
  using Net::ClientAdmit::Outcome;
  ClientCtx* ctx = nullptr;
  const Outcome outcome = Net::ClientAdmit::admit(
      RNS_MAX_CLIENTS,
      [] { return Bootloader::pending(); },
      [this] { return clientCount(); },
      // A refused client may or may not have consumed an id; either is fine,
      // because ids only ever have to be unique. Nothing counts them —
      // sendTo() and clientConnected() both match on the value.
      [this, client]() -> ClientCtx* {
        return new (std::nothrow) ClientCtx{ _nextId++, client, {} };
      },
      [this, &ctx](ClientCtx* c) {
        // Scoped: _lock covers the list, not the registration further down.
        // clientConnected() waits up to 20 ms for room on the RNS task's event
        // queue (postEvent's xQueueSend, in RnsTransport.cpp, which is where
        // that figure is set), and _lock is the lock that same task's sendTo()
        // takes — held across the post it would block the RNS task for that
        // wait.
        {
          Sys::Lock held(_lock);
          _clients.push_back(c);
          g_stats.tcpClients = _clients.size();
        }
        // Last, and only from here. Neither the store above nor this one can
        // throw, which is what keeps the push all-or-nothing.
        ctx = c;
      },
      [](ClientCtx* c) { delete c; },
      [client] { refuse(client); });

  // Every answer but Accepted says so, naming the peer and what happened.
  // Without this a client refused for want of memory could be turned away in
  // silence: the new-handler's own line is rate-limited too (Diag.cpp) and says
  // only that an allocation failed, never that a peer was turned away or which
  // one, and nothing else on this path speaks at all.
  //
  // `client` is not touched again in any of these branches — admit() has
  // already refused it, and the refusal is what freed it.
  //
  // The two answers a starved node gives go through _pressureLog and the other
  // two do not. A peer reconnecting in a tight loop against such a node is
  // refused on every attempt, and each of those two answers costs this task a
  // formatted write — the enrolment one costs it two walks of the heap's free
  // list under the allocator's lock as well, inside noteCaught()
  // (Diag.cpp:158-159). That is per-attempt work on the task that also serves
  // the portal, in the one condition this whole path exists for. The restart
  // and cap answers stay unrated: neither costs anything past the line itself,
  // and a run of them is how a node sitting at its cap is recognised.
  //
  // What is rated is the reporting, and never a count. The refusals are counted
  // whether or not a line comes out, and so is the containment below — the gate
  // is handed to noteCaught() as its `report` argument rather than wrapped
  // around the call.
  //
  // The count in those two lines is the pair's total, not each line's: what an
  // operator needs from it is how many peers were turned away while the node
  // was short, and which of the two answers it gave is already in the text.
  switch (outcome) {
    case Outcome::Accepted:
      break;
    case Outcome::RefusedRestarting:
      log_w("Refusing %s: a restart is pending", remote);
      return;
    case Outcome::RefusedFull:
      log_w("Refusing %s: client limit (%d) reached", remote, RNS_MAX_CLIENTS);
      return;
    case Outcome::RefusedNoMemory:
      if (_pressureLog.due(PRESSURE_LOG_MS))
        log_w("Refusing %s: no room for the %u-byte client context "
              "(%lu refused so far)", remote, (unsigned)sizeof(ClientCtx),
              (unsigned long)_pressureLog.events);
      return;
    case Outcome::RefusedNotEnrolled: {
      const bool due = _pressureLog.due(PRESSURE_LOG_MS);
      // What the list did, not why it did it: enrolling threw, so the client is
      // not in it. The cause was never read here — see below.
      if (due)
        log_w("Refusing %s: the client list would not take it "
              "(%lu refused so far)", remote, (unsigned long)_pressureLog.events);
      // The one refusal that contained a throw, recorded the way every other
      // contained throw in this firmware is. noteCaught() is the half they all
      // go through (Diag.h), so this lands in the same `caught` counter, the
      // same RTC mirror and the same `contained` figure that /api/status
      // (WifiManager.cpp:1824), the console's STATUS (Maintenance.cpp:679-682)
      // and the boot report (Diag.cpp:298-300) already carry — rather than in a
      // parallel count of its own. Called directly because the containment
      // itself belongs to admit(), a pure header with no Diag in it and where
      // the host suite proves it; this is the half a pure header cannot do.
      //
      // Every refusal is counted; `due` decides only whether it is also
      // reported, which is where the heap figures behind the failure come from
      // and which the line above does not repeat. The count must stay exact
      // because `caught` is not the memory counter — allocFailures is, and it
      // does not move for a throw that was not a bad_alloc, which is precisely
      // the case admit()'s broad catch exists for and which
      // test_an_enrolment_that_throws_anything_at_all_is_still_contained pins.
      // Rate the count as well and that containment is recorded nowhere.
      //
      // And `why` says only what was read. admit()'s catch is deliberately
      // broad and discards what it caught, so no reason was ever in hand here;
      // naming one — "the list could not grow" — would be asserting the likely
      // cause rather than reporting the observed one.
      Diag::noteCaught("enrolling a Reticulum client",
                       "it threw; the client was refused and its context freed", due);
      return;
    }
  }

  // Nothing carrying ctx is attached until here, and that is the protection
  // rather than a tidiness: attached earlier, a refusal would free a context
  // the framework still points at, and refuse() replaces only the *disconnect*
  // callback — onData would keep its stale argument and the next byte off the
  // socket would be read through a freed deframer. ClientAdmit.h states the
  // rule; this is the whole of the compliance.
  //
  // Enrolling before attaching costs nothing in return: the RNS task can only
  // name a client by the id clientConnected() gives it, that call has not
  // happened yet, and sendTo() matches on the id — so no send can reach this
  // context in the window. The setters below are plain assignments that cannot
  // throw, and this runs on the single AsyncTCP event task, so no event for
  // this socket can arrive part-way through them.
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

  RnsTransport::clientConnected(ctx->id, remote);
  log_i("Reticulum peer connected: %s (#%lu, %d total)", remote,
        (unsigned long)ctx->id, (int)g_stats.tcpClients);
}

void RetiTransportServer::onDisconnect(ClientCtx* ctx) {
  // Scoped, and the scope ends where the hand-written give did: _lock is not
  // held across clientDisconnected(), which blocks on the RNS task's event
  // queue exactly as clientConnected() does above — the wait is quoted there,
  // once — and _lock is the lock that same task's sendTo() takes.
  {
    Sys::Lock held(_lock);
    for (auto it = _clients.begin(); it != _clients.end(); ++it) {
      if (*it == ctx) { _clients.erase(it); break; }
    }
    g_stats.tcpClients = _clients.size();
  }

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
  // OVERSIZE_LOG_MS, because a desynced peer produces a stream of them, through
  // the same rate limiter the refusals above use. The count in the line stays
  // the deframer's own (per connection, every oversize frame it saw) rather
  // than the limiter's `events`, which counts only the chunks that reached the
  // gate: "so far" has always meant this peer's frames.
  const uint32_t oversized = ctx->deframer.oversized();
  if (oversized != oversizedBefore && _oversizeLog.due(OVERSIZE_LOG_MS)) {
    // Through peerName() for the reason the block above it gives: this is the
    // same event task, and a desynced peer is as likely to be found on a node
    // that is short of memory as anywhere else. It also names the socket rather
    // than only the host, which is what every other line about a peer in this
    // file does.
    char peer[PEER_NAME_MAX];
    peerName(ctx->client, peer, sizeof(peer));
    log_w("%s framed a packet over the %d-byte MTU (%lu so far); it will not be received",
          peer, RNS_MTU, (unsigned long)oversized);
  }
}

// ---------------------------------------------------------------------------
// Outbound: one packet to one client. Called from the RNS task; _lock
// serialises the client list and the framing scratch buffer.
// ---------------------------------------------------------------------------
bool RetiTransportServer::sendTo(uint32_t clientId, const uint8_t* packet, size_t len) {
  bool ok = false;
  // Held for the framing as well as the list, because _frameBuf is shared.
  // Sys::Lock rather than a give at the bottom, because this runs on the RNS
  // task, which runs under Diag::guard (main.cpp): a throw anywhere in here
  // leaves the scope past that give, and a leaked _lock deadlocks the AsyncTCP
  // task on the next connect — silently, which is worse than the restart the
  // guard exists to avoid. Lock.h states the rule.
  Sys::Lock held(_lock);
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
  return ok;
}

