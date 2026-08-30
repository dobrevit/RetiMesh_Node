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
//  ConsoleServer.h — the maintenance console over TCP
//
//  The console (Maintenance.h) answers the same commands on a socket that it
//  answers on the cable: STATUS, LINKS, and GET/SET over all the settings the
//  web API exposes, with the same validation and the same refusals. This file
//  is a transport and nothing else — it accepts a connection, hands the
//  stream to the console and takes it back when the caller goes. The parser,
//  the settings rules and the authentication live where they already lived.
//
//  Why this exists. A node has to be configurable from a distance, and the
//  web portal is an expensive way to be: on a Heltec Wireless Stick the
//  server, its captive DNS and mDNS cost 28616 B of byte-addressable RAM, on
//  a board that finishes booting with about 27 KB of it. The Reticulum TCP
//  listener beside it costs 272 B. A line protocol on a socket is the second
//  of those, not the first — so a board too small to host a portal can still
//  be administered over the network, by a host program rather than a browser.
//
//  It listens on every interface, like the rest of the node's services, so
//  it answers over the access point, the station link, usb0 and ppp0 alike.
//  Every caller authenticates (Maintenance.h): the cable is trusted because
//  physical access is already more than a password, and a socket is not.
//
//  One caller at a time (MAINT_NET_SESSIONS). This is a configuration
//  channel, not a service — two operators writing settings at once is a race
//  nobody asked for.
//
//  Polled from the loop task with the rest of the console. NetworkClient is
//  a Stream and the reads are non-blocking, so there is no task and no
//  cross-task buffer here: the whole transport is an accept() and a
//  disconnect check.
// ============================================================================
#pragma once

#include <stdint.h>

namespace ConsoleServer {

// Applies the switches: listens while maintenance.console_tcp and
// maintenance.console_enabled are both on, and gives the listener back when
// either goes off, so a node that will never be administered over the
// network pays nothing for the possibility. It reads them itself rather than
// being told, for the same reason Maintenance::poll() does — the rule for
// when the console answers has one home. From the loop task, every pass,
// before Maintenance::poll().
void poll();

// Whether the listener is up, and the port it is on.
bool     listening();
uint16_t port();

// Whether a caller is connected, and whether it has authenticated. For
// STATUS and the diagnostics: an operator wants to know if somebody else is
// holding the one session.
bool connected();
bool authenticated();

} // namespace ConsoleServer
