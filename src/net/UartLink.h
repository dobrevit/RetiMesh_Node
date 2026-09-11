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
//  UartLink.h — one UART carrying framed bytes, and nothing above that
//
//  A wire, not a protocol. It hands whole HDLC frames up and takes whole HDLC
//  frames down, and has no idea what is inside them. That separation is the
//  entire reason it exists: the Reticulum serial interface (issue 10) and the
//  remote-radio link (issue 18) both need a UART that does this, and coupling
//  either of them to the port would mean writing the port twice.
//
//  What it owns and what it borrows
//  --------------------------------
//  Owns: the UART, its receive task, and a bounded transmit queue. Borrows
//  everything that already exists and is already tested — `HDLC::Deframer` for
//  framing and resynchronisation (test_hdlc pins ten corruption cases),
//  `Uart::TxQueue` for the bound and the counters (test_uart_tx_queue), and
//  `LocalLink::pppBaudAllowed` for whether a baud is one this board has been
//  qualified at. None of those rules is restated here.
//
//  Off means not allocated
//  -----------------------
//  `begin()` takes the port, the ring, the task and the queue's arena; `end()`
//  gives all of it back — about seven kilobytes, which on a board with ten to
//  spare is the difference between an option and a liability.
//
//  "Holds nothing" is not quite true and the difference is worth stating. On a
//  board that *enables* this, roughly 650 bytes are held whether or not
//  `begin()` ever runs: `HardwareSerial`'s constructor creates a mutex during
//  static initialisation, and the deframer's buffer and the queue's own fields
//  are `.bss`. On every board in `boards.json` the cost really is nothing,
//  because `HAS_SERIAL_LINK` is 0 and this compiles to an empty translation
//  unit — see Config.h for why no board sets it yet.
//
//  The port has other claimants
//  ----------------------------
//  Three things want a UART on this hardware and only one of them can have
//  each port: the console and PPP share the bridge UART through an arbiter
//  (PppArbiter.h), and the GNSS receiver owns UART1 wherever there is one. A
//  board enables this link by naming a port none of them is using, which is a
//  bench question — `begin()` refuses a port that collides with either of the
//  two this firmware can see, but it cannot see a wire that is not plugged in.
//
//  Threading
//  ---------
//  One task reads the port and runs the deframer; it also drains the transmit
//  queue, so the UART has exactly one user and needs no lock of its own. The
//  frame sink is called on that task: a caller that cannot do its work there
//  should copy and return. `send()` is safe from any task — it takes a short
//  spinlock over the queue, the same shape Environment uses for its reading.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_SERIAL_LINK

#include <stdint.h>
#include <stddef.h>
#include "UartTxQueue.h"

namespace UartLink {

// Everything the link has done since boot. The transmit half is the queue's
// own (UartTxQueue.h); the receive half is counted here because only the task
// that runs the deframer can see it.
struct Counters {
  Uart::TxCounters tx;
  uint32_t framesReceived = 0;
  uint32_t bytesReceived  = 0;   // bytes off the wire, framing included
  uint32_t oversized      = 0;   // frames longer than the deframer's buffer
};
//
// Two counters the roadmap asks for are deliberately absent rather than
// present and always zero: resynchronisations, and receive-ring overruns.
// Neither is observable from here today — `HDLC::Deframer` cannot tell a
// truncated frame from a complete one, having no checksum to fail, and
// `HardwareSerial` does not surface the driver's ring overflow. Both want a
// change to a component with tests of its own, which is a second change.
// A field that reads zero for ever is worse than a missing one: a surface
// would print it as a measurement.

// A whole frame, on the receive task. Copy it if it must outlive the call.
using FrameSink = void (*)(const uint8_t* frame, size_t len, void* ctx);

// Take the port at `baud` and start reading. False when the baud is not one
// this board is qualified for, when the port is one the console, PPP or the
// GNSS receiver already owns, or when the memory could not be found — each of
// which is logged with which it was. Safe to call when already up: it is a
// no-op that answers true.
bool begin(uint32_t baud, FrameSink sink, void* ctx);

// Give the port, the task, the ring and the queue's arena back. Safe to call
// when already down. The counters survive, because a link switched off has not
// un-dropped what it dropped.
//
// False when the reader would not stop inside a second, in which case nothing
// is freed and the link is still holding its port — the alternative is freeing
// an arena under a running task. Blocks for up to that second, so it must not
// be called from RnsTransport::loop().
bool end();

bool     up();
uint32_t baud();

// Queue a frame. False means the queue was full and the frame was dropped and
// counted — backpressure the caller can see, rather than a slow link quietly
// becoming a lossy one. Safe from any task.
bool send(const uint8_t* frame, size_t len);

// A snapshot, safe from any task.
Counters counters();

// The bauds this board says it has been qualified at, for a surface that
// offers them. The list is the registry's (boards.json local_link.uart) and is
// not restated here.
const uint32_t* qualifiedBauds(size_t& count);

} // namespace UartLink

#else   // !HAS_SERIAL_LINK

#include <stdint.h>
#include <stddef.h>
#include "UartTxQueue.h"

namespace UartLink {
struct Counters {
  Uart::TxCounters tx;
  uint32_t framesReceived = 0, bytesReceived = 0, oversized = 0;
};
using FrameSink = void (*)(const uint8_t*, size_t, void*);
inline bool begin(uint32_t, FrameSink, void*) { return false; }
inline bool end() { return true; }
inline bool up() { return false; }
inline uint32_t baud() { return 0; }
inline bool send(const uint8_t*, size_t) { return false; }
inline Counters counters() { return Counters{}; }
inline const uint32_t* qualifiedBauds(size_t& count) { count = 0; return nullptr; }
} // namespace UartLink

#endif  // HAS_SERIAL_LINK
