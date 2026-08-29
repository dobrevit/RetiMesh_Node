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
//  PppUart.h — PPP over the bridge UART: ppp0
//
//  On the boards whose USB connector is a serial bridge (HAS_PPP: the
//  Heltec V3, Wireless Stick and Wireless Bridge on a CP2102, the T-Beam
//  on a CH9102) the one serial port can carry IP as well as text: the host
//  runs pppd on /dev/ttyUSB0 and the node becomes a network interface at
//  the other end of it. Every service already binds every interface, so
//  http://10.65.<n>.1/, the Reticulum TCP transport and the bootloader API
//  answer over the wire as they do over Wi-Fi.
//
//  The node is the PPP client and the host's pppd is the server. That is
//  the opposite of the USB link's shape — usb0 serves DHCP and hands the
//  host its address — and it is not a choice: the core's prebuilt lwIP has
//  CONFIG_LWIP_PPP_SUPPORT but not CONFIG_LWIP_PPP_SERVER_SUPPORT, so the
//  node cannot wait for a peer or assign one an address. What it can do is
//  ask. IPCP lets a client request its own address, and the node asks for
//  pppNodeAddress(<last MAC octet>) — 10.65.<n>.1, the rule in
//  LocalLinkState.h — while the host's pppd is told to take .2 and offer .1:
//
//      pppd /dev/ttyUSB0 115200 noauth local nodetach lcp-echo-interval 5 \
//           nocrtscts lcp-echo-interval 5 lcp-echo-failure 4 10.65.<n>.2:10.65.<n>.1
//
//  When the two agree nothing is negotiated at all; when the host chooses
//  otherwise the node takes what it is given and reports it, since the
//  peer decides on a client. The addressing is "ipcp" in the API for that
//  reason, not "static".
//
//  The port is shared. UART0 carries the log, the maintenance console and,
//  once the host opens it, PPP — and a log line inside an HDLC frame is a
//  corrupt frame. So the port has one owner at a time (PppArbiter.h):
//
//    Console   the log prints, the console answers; every received byte
//              is watched for pppd's opening LCP Configure-Request
//    Ppp       the host sent one: the log is muted at its source (the
//              core's putc, ESP-IDF's vprintf, microReticulum's level), the
//              console stops reading and its replies are dropped, and every
//              byte goes to lwIP's PPP. Back to Console when LCP finishes —
//              pppd exits, or the operator switches the link off — or when
//              the host has sent no frame for kIdleDeadMs.
//
//  The reader is one task on core 0, below the radio task in priority, that
//  blocks on the UART driver and never on anything else. The driver's own
//  receive ring is the PPP receive ring, PPP_RX_RING_BYTES deep; the driver
//  drops what does not fit and PPP retransmits, so the radio never waits
//  for the serial port. Transmit gathers a frame from lwIP's output and
//  writes it in one piece into a PPP_TX_QUEUE_BYTES queue, dropping a frame
//  whole when the queue cannot take it. RTS/CTS are not driven: no board
//  here brings the bridge's lines to the chip (boards.json uart.rts/cts),
//  and the hooks are the two settings in this header's .cpp that would
//  turn them on.
//
//  The speed is settings.links.pppBaud, and it is the speed of the whole
//  port — the console and the log included — while links.ppp is on. The
//  default is the console's 115200, so switching PPP on changes nothing a
//  host already relies on; a faster rate is refused unless the board's
//  registry entry lists it and it is no higher than the rate the board has
//  been tried at (LocalLinkState.h, pppBaudAllowed).
// ============================================================================
#pragma once

#include <stdint.h>
#include "Config.h"

#if HAS_PPP
#include <Arduino.h>
#include <IPAddress.h>
#include "PppArbiter.h"

namespace PppUart {

// Creates the network interface and starts the UART reader. Once, after the
// network stack exists (WifiManager::begin() brings it up whether or not
// Wi-Fi is on) and after settings are loaded.
void begin();

// Applies the operator's switch and speed, from the loop task. The switch
// is honoured within a pass of the reader; the speed when the console next
// owns the port, so a change saved over ppp0 does not cut the session that
// asked for it.
void poll(bool enabled, uint32_t baud);

// Who has the port now.
Owner owner();

// IPCP is done: the node is reachable at address() from peer().
bool linkUp();
IPAddress address();
IPAddress peer();

// What the node asks for — pppNodeAddress(<n>) — and the address the rule
// gives the host, for the console and the API to tell a host what to
// give its pppd. The session's real addresses are the two above.
IPAddress askedAddress();
IPAddress askedPeer();

// Close a session the node is about to restart under, and wait up to
// `waitMs` for the port to come back to the console, so that the host's
// pppd sees LCP end rather than a line going dead. From the loop task, in
// the restart's quiesce step; a no-op when the console has the port.
void shutdown(uint32_t waitMs);

// The speed the port runs at.
uint32_t baud();

// The stream the maintenance console reads and writes. Bytes the host
// sends while the console owns the port arrive here; what the console
// writes reaches the port while it owns it and is dropped while PPP does.
Stream& console();

// Bytes moved and frames lost, and how many times a host has opened PPP.
uint32_t rxBytes();
uint32_t txBytes();
uint32_t txDropped();
uint32_t sessions();

} // namespace PppUart
#endif
