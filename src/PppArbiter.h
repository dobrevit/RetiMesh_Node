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
//  PppArbiter.h — who owns the bridge UART: the console, or PPP
//
//  On the boards with a USB-UART bridge there is one serial port and three
//  things that want it: the log, the maintenance console and — when a host
//  runs pppd on the other end — a PPP link. The first two are text, the
//  third is HDLC frames, and a log line landing inside a frame corrupts it.
//  So the port has one owner at a time, and this is the rule that decides
//  which:
//
//      Console ──(host sends an LCP Configure-Request)──► Ppp
//      Ppp ──(LCP terminates, or no frame for kIdleDeadMs)──► Console
//
//  While the console owns the port every byte is the console's, except that
//  a 0x7E — the HDLC flag, '~' in ASCII, which no console command uses — is
//  held back with what follows it until it is clear whether a PPP frame is
//  starting. pppd opens with an LCP Configure-Request, and its first bytes
//  on the wire are unmistakable once unescaped: FF 03 (address, control)
//  C0 21 (LCP) 01 (Configure-Request). Anything else — a '~' somebody typed,
//  a frame of another kind, a candidate the port goes quiet on — is released
//  to the console as it was. The held bytes are handed to PPP in order when
//  it takes over, so the frame that opened the session is not lost to the
//  hand-over.
//
//  While PPP owns the port nothing here looks inside the bytes; lwIP's PPP
//  does that. The arbiter only keeps time: a link on which the host has sent
//  no frame for kIdleDeadMs is dead — pppd was killed, the cable was pulled —
//  and the port goes back to the console so the node is not left silent on
//  its one serial port. A live pppd with `lcp-echo-interval` set sends a
//  frame every few seconds whether or not there is traffic, which is what
//  the documented pppd command line asks for.
//
//  Pure: no Arduino, no lwIP, no task. PppUart.cpp feeds it bytes and acts on
//  its answers; test_ppp_uart exercises it on the host.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace PppUart {

enum class Owner : uint8_t { Console = 0, Ppp };

inline const char* ownerName(Owner o) { return o == Owner::Ppp ? "ppp" : "console"; }

// Where feed() says a byte — or a run of bytes it had been holding — goes.
enum class Sink : uint8_t { Hold = 0, Console, Ppp };

struct Route {
  Sink           sink;
  const uint8_t* data;      // valid until the next call on the arbiter
  size_t         len;
  bool           tookOver;  // this byte decided it: PPP owns the port from here
};

class Arbiter {
public:
  // A Configure-Request is decided within 11 raw bytes (the flag, then five
  // bytes each possibly escaped); anything held longer than this is not one.
  static constexpr size_t   kHoldMax    = 16;
  // A held candidate the port goes quiet on for this long is given up: a
  // typed '~' is not the start of a frame that never comes.
  static constexpr uint32_t kHoldIdleMs = 500;
  // No frame from the host for this long and the session is dead. Six of
  // the five-second LCP echoes the documented pppd invocation sends.
  static constexpr uint32_t kIdleDeadMs = 30000;

  explicit Arbiter(bool pppAllowed = false) : _allow(pppAllowed) {}

  Owner owner() const { return _owner; }

  // Whether a frame may take the port at all — the operator's switch. Off
  // while a candidate is held means the candidate goes to the console.
  void allowPpp(bool allowed) { _allow = allowed; }
  bool pppAllowed() const { return _allow; }

  // A byte received while the console owns the port.
  Route feed(uint8_t c, uint32_t nowMs) {
    if (_owner == Owner::Ppp) { _one = c; return { Sink::Ppp, &_one, 1, false }; }
    _lastByteMs = nowMs;
    if (!_holding) {
      if (c == kFlag && _allow) {
        _holding = true; _nHeld = 0; _nDecoded = 0; _esc = false;
        _held[_nHeld++] = c;
        return hold();
      }
      _one = c;
      return { Sink::Console, &_one, 1, false };
    }
    _held[_nHeld++] = c;
    if (c == kFlag) {
      // Flags back to back are still a frame start; a flag after content
      // ends a frame that never became a Configure-Request.
      if (_nDecoded == 0 && !_esc) return _nHeld < kHoldMax ? hold() : release();
      return release();
    }
    if (c == kEscape) {
      if (_esc) return release();
      _esc = true;
      return _nHeld < kHoldMax ? hold() : release();
    }
    _decoded[_nDecoded++] = _esc ? (uint8_t)(c ^ 0x20) : c;
    _esc = false;
    if (!prefixOk()) return release();
    if (complete()) {
      if (!_allow) return release();
      _owner = Owner::Ppp;
      _holding = false;
      _lastFrameMs = nowMs;
      return { Sink::Ppp, _held, _nHeld, true };
    }
    return _nHeld < kHoldMax ? hold() : release();
  }

  // Nothing received lately: a candidate the port went quiet on is released.
  Route idle(uint32_t nowMs) {
    if (_owner == Owner::Console && _holding && nowMs - _lastByteMs > kHoldIdleMs) return release();
    return hold();
  }

  // Bytes received while PPP owns the port. Only the clock is kept: a flag
  // among them is a frame, and a frame is proof the host is there.
  void pppReceived(const uint8_t* data, size_t len, uint32_t nowMs) {
    if (_owner == Owner::Ppp && data && memchr(data, kFlag, len)) _lastFrameMs = nowMs;
  }

  // No frame from the host for kIdleDeadMs: the driver should close the
  // session and hand the port back.
  bool pppIdleDead(uint32_t nowMs) const {
    return _owner == Owner::Ppp && nowMs - _lastFrameMs > kIdleDeadMs;
  }

  // The session ended — LCP finished, or the driver closed it. True when the
  // port changed hands.
  bool pppDown() {
    if (_owner != Owner::Ppp) return false;
    _owner = Owner::Console;
    _holding = false;
    _nHeld = 0;
    return true;
  }

private:
  static constexpr uint8_t kFlag   = 0x7E;
  static constexpr uint8_t kEscape = 0x7D;

  Route hold() { return { Sink::Hold, nullptr, 0, false }; }
  Route release() { _holding = false; return { Sink::Console, _held, _nHeld, false }; }

  // The unescaped bytes so far are the start of one of the two spellings of
  // an LCP Configure-Request: with the address/control pair pppd always
  // sends on LCP, or without it.
  bool prefixOk() const {
    static const uint8_t kWithAc[5] = { 0xFF, 0x03, 0xC0, 0x21, 0x01 };
    static const uint8_t kBare[3]   = { 0xC0, 0x21, 0x01 };
    return prefixOf(kWithAc, sizeof(kWithAc)) || prefixOf(kBare, sizeof(kBare));
  }
  bool complete() const {
    return (_nDecoded == 5 && _decoded[0] == 0xFF) || (_nDecoded == 3 && _decoded[0] == 0xC0);
  }
  bool prefixOf(const uint8_t* seq, size_t n) const {
    if (_nDecoded > n) return false;
    return memcmp(_decoded, seq, _nDecoded) == 0;
  }

  Owner    _owner = Owner::Console;
  bool     _allow;
  bool     _holding = false;
  bool     _esc = false;
  uint8_t  _held[kHoldMax + 1];
  size_t   _nHeld = 0;
  uint8_t  _decoded[5];
  size_t   _nDecoded = 0;
  uint8_t  _one = 0;
  uint32_t _lastByteMs = 0;
  uint32_t _lastFrameMs = 0;
};

} // namespace PppUart
