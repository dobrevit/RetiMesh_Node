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

// ConsoleServer.cpp — see ConsoleServer.h. A listener, one client slot, and
// the moments that matter: a caller arrives, a caller goes, a caller has
// stopped saying anything, a caller has stopped reading.
#include "ConsoleServer.h"

#include <Arduino.h>
#include <NetworkServer.h>
#include <NetworkClient.h>
#include <errno.h>
#include <sys/socket.h>            // the bounded write below; the read path is NetworkClient's

#include "Config.h"
#include "Settings.h"
#include "LocalLink.h"
#include "Maintenance.h"
#include "MaintenanceProtocol.h"

namespace ConsoleServer {

// The listener is built when the switch turns on and destroyed when it turns
// off, rather than being left bound and silent: off should mean the socket
// does not exist (Config.h).
static NetworkServer* sServer = nullptr;
static NetworkClient  sClient;
static bool           sHaveClient = false;
static uint32_t       sLastByteMs = 0;
static uint32_t       sOpenedMs = 0;

// A caller that connects and says nothing holds the only session there is.
// Long enough that an operator can read a reply and think, short enough that
// a forgotten terminal does not lock the node's configuration channel for
// the rest of the day.
static constexpr uint32_t kIdleMs = 120000;
// A caller that has not authenticated gets an absolute deadline instead, not
// one its own traffic pushes back: a byte a minute would otherwise hold the
// console for ever without ever offering a password.
static constexpr uint32_t kUnauthedMs = 20000;

// How long a reply may wait for room in the socket's send buffer. Bounded
// for the reason every other write in this firmware is bounded: this runs on
// the loop task, which also carries the restart sequencer, the links and the
// heartbeat, and the framework's own write() blocks for up to ten seconds a
// call when the peer stops reading. A host that has stopped reading loses
// the tail of its reply; it does not get to stop the node.
static constexpr uint32_t kWriteWaitMs = 50;

// The client dressed as the Stream the console writes through, so the bound
// above applies to every reply rather than to the places somebody remembered.
class ClientStream : public Stream {
public:
  int  available() override { return sClient.available(); }
  int  read() override      { return sClient.read(); }
  int  peek() override      { return sClient.peek(); }
  void flush() override     {}

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    const int fd = sClient.fd();
    if (fd < 0) return len;
    size_t sent = 0;
    const uint32_t started = millis();
    while (sent < len) {
      const int n = ::send(fd, data + sent, len - sent, MSG_DONTWAIT);
      if (n > 0) { sent += (size_t)n; continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (millis() - started >= kWriteWaitMs) break;   // not reading: drop the rest
        delay(1);
        continue;
      }
      break;                                             // closed, or reset
    }
    // Reported as sent either way: the console has no answer to "the host
    // stopped listening" beyond carrying on, and a short count would only
    // make the caller above try again.
    return len;
  }
};
static ClientStream sStream;

bool     listening() { return sServer != nullptr; }
uint16_t port()      { return CONSOLE_TCP_PORT; }
bool     connected() { return sHaveClient; }
bool     authenticated() { return sHaveClient && Maintenance::sessionAuthed(sStream); }

// Whether the peer has gone, asked of the client itself.
//
// A raw MSG_PEEK on the socket looks like a more direct question and is the
// wrong one: NetworkClient buffers its reads, and a peek beside that racing
// reader makes it take an empty socket for a failure, close its own
// descriptor and report a caller that is still there as hung up. Measured —
// a session that sent anything at all was dropped within ten seconds.
//
// So the client is asked, and the timeouts in poll() are the backstop rather
// than this. If a future core stops noticing a peer's FIN here, an
// unauthenticated caller still goes at kUnauthedMs and an authenticated one
// at kIdleMs, so the worst case is a slot held for two minutes and never a
// slot held for ever. Verified on this core: ten back-to-back sessions, each
// closing cleanly, all accepted.
static bool peerGone() {
  return !sClient.connected();
}

// A caller turned away, in the protocol's own words (MaintenanceProtocol.h)
// rather than a line spelled out here: one format, one place it comes from.
static void refuse(NetworkClient& c, const char* why) {
  char line[128];
  const size_t n = Maintenance::formatErr(line, sizeof(line), "?", 503, why);
  c.write((const uint8_t*)line, n < sizeof(line) ? n : sizeof(line) - 1);
  c.write('\n');
  c.flush();                                   // on the wire before the close, or the caller reads a reset
  c.stop();
}

static void drop(const char* why) {
  if (!sHaveClient) return;
  Maintenance::closeSession(sStream);
  sClient.stop();
  sHaveClient = false;
  log_i("console: the network session ended (%s)", why);
}

static void start() {
  sServer = new NetworkServer(CONSOLE_TCP_PORT);
  if (!sServer) { log_e("console: no listener for port %d", CONSOLE_TCP_PORT); return; }
  sServer->begin(CONSOLE_TCP_PORT);
  // Asked after begin(), which is the only time it means anything: begin()
  // clears the server's own flag on its way out. The accepted socket is set
  // again below in any case, which is the one that carries a reply.
  if (!*sServer) {
    // A bind that failed must not read as a console that is listening: with
    // the handle kept, poll() would never build another and STATUS would say
    // the port answers when nothing is bound.
    log_e("console: could not bind port %d; nothing is listening", CONSOLE_TCP_PORT);
    delete sServer;
    sServer = nullptr;                         // so the next pass tries again
    return;
  }
  sServer->setNoDelay(true);
  log_i("console: listening on 0.0.0.0:%d — AUTH with the admin password (ConsoleServer.h)",
        CONSOLE_TCP_PORT);
}

static void stop() {
  drop("the console was switched off");
  if (sServer) {
    sServer->end();
    delete sServer;
    sServer = nullptr;
    log_i("console: no longer listening on TCP");
  }
}

void poll() {
  // Both switches, and the console's own comes first: with the console off
  // there is nothing behind the socket to answer, and a port that accepts a
  // connection and then says nothing is worse than a closed one.
  const MaintenanceSettings& m = settings.maintenance();
  const bool want = m.consoleEnabled && m.consoleTcp;
  if (want && !sServer)      start();
  else if (!want && sServer) stop();
  if (!sServer) return;

  if (sHaveClient) {
    const uint32_t now = millis();
    if (peerGone())                                drop("the caller hung up");
    else if (Maintenance::sessionClosing(sStream)) drop("too many failed passwords");
    else if (!Maintenance::sessionAuthed(sStream) && now - sOpenedMs > kUnauthedMs)
      drop("no password within the grace");
    else {
      if (sClient.available() > 0) sLastByteMs = now;
      if (now - sLastByteMs > kIdleMs)             drop("idle");
    }
  }

  if (!sServer->hasClient()) return;
  NetworkClient caller = sServer->accept();
  if (!caller) return;
  // Asked again the moment a new caller arrives, because that is a different
  // instant: a script that runs one command per connection closes and
  // reconnects inside a single pass of the loop task, and judging the
  // newcomer against a session that ended microseconds ago turned away every
  // second command.
  if (sHaveClient && peerGone()) drop("the caller hung up");
  if (sHaveClient) {
    // One caller at a time — this is a configuration channel, not a service,
    // and two operators writing settings at once is a race nobody asked for.
    // Said rather than dropped silently, so whoever is dialling in knows it
    // is not broken.
    refuse(caller, "another session has the console");
    return;
  }
  sClient = caller;
  sClient.setNoDelay(true);                    // a line at a time; Nagle would sit on a reply
  // Whether this caller is on a link the node is the host end of — the
  // access point, usb0, ppp0 — or out on the station network. The bootloader
  // asks the same of an HTTP request and gets it from the same rule
  // (LocalLink.h), because a command that drops a relay into its ROM must
  // not be easier to reach over the console than over the API.
  const bool hostFacing = LocalLink::requestIsHostFacing(
      LocalLink::hostOrder(sClient.localIP()), LocalLink::hostOrder(sClient.remoteIP()));
  if (!Maintenance::openSession(sStream, hostFacing)) {
    refuse(sClient, "no session available");
    return;
  }
  sHaveClient = true;
  sOpenedMs = sLastByteMs = millis();
  // No HELLO: that line is the cable's, where it tells a flashing tool the
  // node is alive. Here the caller opened the connection and knows.
  log_i("console: a network session opened from %s (%s)", sClient.remoteIP().toString().c_str(),
        hostFacing ? "a host-facing link" : "the station network");
}

} // namespace ConsoleServer
