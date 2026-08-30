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
//  CaptiveDns.h — the access point's resolver, and only the access point's
//
//  A captive portal works by answering every name with the portal's own
//  address, so a phone's connectivity probe lands on port 80 and the
//  "sign in" sheet appears. That answer is right on the access point and
//  wrong everywhere else: the USB link's DHCP lease names the node as a DNS
//  server too (ESP-IDF's server names itself when told to name nobody), and
//  a host that asked the node over the cable would have every name steered
//  to the portal. The core's DNSServer answers on every interface and keeps
//  its socket to itself, so this is a responder of our own that looks at
//  which link a query arrived over: the access point's gets the portal's
//  answer, any other gets REFUSED — an answer, and at once, which is what
//  lets the host's resolver move on rather than wait out a timeout.
//
//  "Arrived over" is both addresses, not the local one alone. lwIP accepts a
//  packet addressed to any of the node's own addresses whichever interface it
//  turns up on, so a host on the LAN or on the USB cable that sends to the
//  access point's address reaches this responder with the access point's
//  address as the local one. The sender has to be on the access point's own
//  subnet too — LocalLink::requestIsOnItsLink, the same rule the bootloader
//  API is guarded by.
//
//  Asynchronous — the reply is built in the receive callback — so there is
//  no polling task, and nothing to poll when the access point is off.
// ============================================================================
#pragma once

#include <AsyncUDP.h>
#include <IPAddress.h>

class CaptiveDns {
public:
  // Listen on port 53; answer every A query that arrives at `ip` with `ip`,
  // and refuse every query that arrives anywhere else.
  bool begin(const IPAddress& ip, uint32_t ttlSeconds = 60);
  void end();
  bool listening() { return _udp.connected(); }         // the socket is bound

private:
  void handle(AsyncUDPPacket& pkt);
  AsyncUDP  _udp;
  IPAddress _ip;
  uint32_t  _ttl = 60;
};
