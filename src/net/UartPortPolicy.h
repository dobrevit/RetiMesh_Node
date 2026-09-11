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
//  UartPortPolicy.h — whether a UART is already somebody's
//
//  Three things on this hardware want a serial port and each port can belong
//  to only one of them, so a link that takes the wrong one does not fail: it
//  works, and breaks the console or the GNSS receiver instead, on a board
//  nobody is looking at.
//
//  Extracted from UartLink because it was a decision with no way to test it.
//  Everything in that file is behind HAS_SERIAL_LINK, which no board sets, and
//  the only env that does turn it on ignores tests — so the one rule there
//  that could be wrong without anybody noticing had no path to a suite at all.
//  Taking the facts as parameters rather than reading macros is what makes it
//  reachable; it is also what caught the bug below.
//
//  The bug this was extracted with
//  ------------------------------
//  The version inside the driver tested `BOARD_UART_INSTANCE` when PPP was
//  compiled in and a bare literal `0` when it was not. Those agree on every
//  board today, which is why it read as correct — and `Config.h` says the
//  instance is "0 on every board **so far**". On the first board with a
//  console on UART2 and no PPP, the serial link would have been handed the
//  console's own port. The console owns the bridge UART whether or not PPP
//  exists to take turns with it; that is the whole rule, and it is one line.
// ============================================================================
#pragma once

namespace UartPort {

// The ports a board has spoken for. `gpsUart` is only consulted when `hasGps`,
// so a board without a receiver does not have to invent a number for one.
struct Owners {
  int  consoleUart;      // the bridge UART: the console's, and PPP's by turns
  bool hasGps;
  int  gpsUart;
};

// Whether `candidate` already belongs to something.
//
// PPP is deliberately not a parameter. It shares the bridge UART with the
// console through an arbiter rather than owning it instead of the console, so
// whether PPP is compiled in changes nothing about whether that port is free:
// it never is.
inline bool taken(int candidate, const Owners& o) {
  if (candidate < 0) return true;          // not a port at all
  if (candidate == o.consoleUart) return true;
  if (o.hasGps && candidate == o.gpsUart) return true;
  return false;
}

} // namespace UartPort
