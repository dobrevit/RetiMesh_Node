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
//  Maintenance.h — the console on the serial port
//
//  The port a host has anyway — the S3's USB CDC, the Heltec's CP2102 —
//  carried only the log. Now it answers: VERSION, STATUS, the state of every
//  local link, and the two commands a flashing tool needs, RESET CONFIRM and
//  BOOTLOADER CONFIRM. The wire format is in MaintenanceProtocol.h and is the
//  same one the CDC-ACM function of the composite USB device will speak,
//  so a host script written against this port keeps working there.
//
//  Log lines share the port. Replies all begin with "RM ", log lines never
//  do, and a request line is never echoed, so a host filters on the prefix.
//  Runs on the loop task, a few bytes per pass; it never blocks and never
//  allocates.
//
//  **More than one way in.** The port is one session; a network transport
//  (ConsoleServer.h) brings its own, and both are served in the same pass.
//  They differ in one thing only:
//
//    the cable    trusted on sight. Physical access already allows dumping
//                 the firmware and reflashing it, which is strictly more
//                 than editing a setting, and the console already answers
//                 BOOTLOADER CONFIRM. So there is no password on the wire.
//    the network  answers HELP, VERSION and AUTH, and nothing else until
//                 AUTH succeeds. A socket on an open access point is not
//                 physical access and the reasoning above does not carry
//                 across it. The credential is the admin password — the
//                 same one the web API takes, so there is one to change and
//                 not two. Failures are counted for the node rather than
//                 the connection, because a limit a caller resets by
//                 hanging up is not a limit; the cable is never locked out,
//                 so no lockout can strand the operator.
// ============================================================================
#pragma once

#include <Arduino.h>

namespace Maintenance {

void begin(Stream& io);            // announces itself with "RM HELLO ..."

// Point the console at another stream. The port is one thing and who hands
// its bytes over is another: with PPP off the console reads the UART itself,
// and while PPP's reader has the port it arbitrates the bytes, so it becomes
// the console's source instead (PppUart.h). Called from the loop task,
// between polls, so no read is in progress. Says nothing on the way past: a
// second HELLO would read as a node that had restarted, and the port has not
// changed — only who hands its bytes over.
void useStream(Stream& io);
void poll();                       // from loop(); reads what has arrived, answers complete lines

// --- sessions a network transport brings ------------------------------------
// The transport owns the socket and its lifetime; the console owns the
// protocol and the authentication. openSession() returns false when every
// slot is taken, and the transport turns the caller away.
bool   openSession(Stream& io);
void   closeSession(Stream& io);
// True when the console is finished with this caller and the transport
// should hang up: too many failed AUTHs. Checked after each poll().
bool   sessionClosing(Stream& io);
bool   sessionAuthed(Stream& io);
size_t openSessions();

} // namespace Maintenance
