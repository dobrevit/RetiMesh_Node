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
//  HDLC.h — the framing Reticulum uses on TCP interfaces
//
//  RNS.Interfaces.TCPInterface wraps every packet in HDLC-style framing:
//
//      FLAG  <escaped packet bytes>  FLAG
//
//  with  0x7E -> 0x7D 0x5E  and  0x7D -> 0x7D 0x5D  inside the frame.
//
//  Implementing this byte-exactly is what lets stock Sideband / rnsd
//  clients connect to port 4242 with a plain TCPClientInterface — the
//  node never needs to understand the (encrypted) packet contents.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace HDLC {

constexpr uint8_t FLAG     = 0x7E;
constexpr uint8_t ESC      = 0x7D;
constexpr uint8_t ESC_MASK = 0x20;

// Worst case every byte escapes: 2 flags + 2*len.
constexpr size_t frameCapacity(size_t len) { return 2 + 2 * len; }

// Frame `in` into `out`; returns bytes written (0 if out is too small).
inline size_t frame(const uint8_t* in, size_t len, uint8_t* out, size_t outCap) {
  if (outCap < frameCapacity(len)) return 0;
  size_t o = 0;
  out[o++] = FLAG;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = in[i];
    if (b == FLAG || b == ESC) {
      out[o++] = ESC;
      out[o++] = b ^ ESC_MASK;
    } else {
      out[o++] = b;
    }
  }
  out[o++] = FLAG;
  return o;
}

// Streaming deframer — one instance per TCP client, fed a byte at a time.
// Calls sink(buf, len) for every complete, non-empty, MTU-sized-or-smaller
// frame. Oversized frames are dropped rather than parsed (a desynced or
// hostile peer must not be able to wedge the parser), and counted: a client
// speaking a larger MTU than this node's looks exactly like a working one
// whose messages never arrive, and the count is the only thing that says
// which of the two is happening.
class Deframer {
public:
  template <typename Sink>
  void feed(uint8_t b, Sink sink) {
    if (b == FLAG) {
      // A FLAG both terminates a frame and opens the next one, so
      // back-to-back FLAGs (empty frames) are simply ignored.
      if (inFrame && len > 0 && !overflow) sink(buf, len);
      if (overflow) dropped++;
      inFrame  = true;
      escaped  = false;
      overflow = false;
      len      = 0;
      return;
    }
    if (!inFrame) return;              // garbage between frames
    if (b == ESC) { escaped = true; return; }
    if (escaped)  { b ^= ESC_MASK; escaped = false; }
    if (len >= sizeof(buf)) { overflow = true; return; }
    buf[len++] = b;
  }

  void reset() { inFrame = false; escaped = false; overflow = false; len = 0; }

  // Frames dropped for exceeding the MTU, since this deframer was created.
  uint32_t oversized() const { return dropped; }

private:
  uint8_t  buf[RNS_MTU];
  size_t   len      = 0;
  uint32_t dropped  = 0;
  bool     inFrame  = false;
  bool     escaped  = false;
  bool     overflow = false;
};

} // namespace HDLC
