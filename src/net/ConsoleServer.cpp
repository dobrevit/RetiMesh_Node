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
// the three moments that matter: a caller arrives, a caller goes, a caller
// has stopped saying anything.
#include "ConsoleServer.h"

#include <Arduino.h>
#include <NetworkServer.h>
#include <NetworkClient.h>

#include "Config.h"
#include "Settings.h"
#include "Maintenance.h"
#include "MaintenanceProtocol.h"

namespace ConsoleServer {

// The listener is built when the switch turns on and destroyed when it turns
// off, rather than being left bound and silent: off should mean the socket
// does not exist (Config.h).
static NetworkServer* sServer = nullptr;
static NetworkClient  sClient;                  // the address is handed to the console; keep it stable
static bool           sHaveClient = false;
static uint32_t       sLastByteMs = 0;

// A caller that connects and says nothing holds the only session there is.
// Long enough that an operator can read a reply and think, short enough that
// a forgotten terminal does not lock the node's configuration channel for
// the rest of the day. It also bounds a caller that never authenticates.
static constexpr uint32_t kIdleMs = 120000;

bool     listening() { return sServer != nullptr; }
uint16_t port()      { return CONSOLE_TCP_PORT; }
bool     connected() { return sHaveClient; }
bool     authenticated() { return sHaveClient && Maintenance::sessionAuthed(sClient); }

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

// Whether the session in hand is still worth holding the slot for. Asked
// both on the ordinary pass and again the moment a new caller arrives,
// because those are different instants: a script that runs one command per
// connection closes and reconnects inside a single pass of the loop task,
// and judging the newcomer against a session that ended microseconds ago
// turned away every second command.
static bool clientAlive() {
  return sHaveClient && sClient.connected();
}

static void drop(const char* why) {
  if (!sHaveClient) return;
  Maintenance::closeSession(sClient);
  sClient.stop();
  sHaveClient = false;
  log_i("console: the network session ended (%s)", why);
}

static void start() {
  sServer = new NetworkServer(CONSOLE_TCP_PORT);
  if (!sServer) { log_e("console: no listener for port %d", CONSOLE_TCP_PORT); return; }
  // A line at a time in each direction: waiting to fill a segment would sit
  // on a reply the caller is blocked reading.
  sServer->setNoDelay(true);
  sServer->begin(CONSOLE_TCP_PORT);
  log_i("console: listening on 0.0.0.0:%d — AUTH with the admin password (ConsoleServer.h)",
        CONSOLE_TCP_PORT);
}

static void stop() {
  drop("the console was switched off");
  if (sServer) {
    sServer->end();
    delete sServer;
    sServer = nullptr;
  }
  log_i("console: no longer listening on TCP");
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
    if (!clientAlive())                        drop("the caller hung up");
    else if (Maintenance::sessionClosing(sClient)) drop("too many failed passwords");
    else {
      if (sClient.available() > 0) sLastByteMs = millis();
      if (millis() - sLastByteMs > kIdleMs)    drop("idle");
    }
  }

  if (!sServer->hasClient()) return;
  NetworkClient caller = sServer->accept();
  if (!caller) return;
  if (sHaveClient && !clientAlive()) drop("the caller hung up");
  if (sHaveClient) {
    // One at a time (ConsoleServer.h). Say so rather than dropping the
    // connection silently, so whoever is dialling in knows it is not broken —
    // in the reply format every other refusal uses, since a caller parsing
    // one shape should not meet a second one here.
    refuse(caller, "another session has the console");
    return;
  }
  sClient = caller;
  sClient.setNoDelay(true);
  if (!Maintenance::openSession(sClient)) {    // cannot happen with one slot; refuse rather than assume
    refuse(sClient, "no session available");
    return;
  }
  sHaveClient = true;
  sLastByteMs = millis();
  // No HELLO: that line is the cable's, where it tells a flashing tool the
  // node is alive. Here the caller opened the connection and knows.
  log_i("console: a network session opened from %s", sClient.remoteIP().toString().c_str());
}

} // namespace ConsoleServer
