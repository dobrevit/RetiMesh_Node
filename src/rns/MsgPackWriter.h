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
//  MsgPackWriter.h — writing msgpack, for the one place that needs more than a
//  fixed shape
//
//  Named for what it does rather than for the format, because <MsgPack.h> is
//  already taken: microReticulum includes the hideakitai library by that name
//  and -I src/rns is on every build here, so a file called MsgPack.h in this
//  directory is one include-order change away from being handed to a
//  dependency that wanted the other one.
//
//  LxmfFormat.h writes the two payloads this node used to send by laying out
//  their bytes directly, which is right when the shape never varies. Telemetry
//  is not that: what a node has to report depends on the board, on whether a
//  cell is connected, on whether a receiver has a fix — so the message is a
//  different shape every time, and hand-placed bytes would become a maze of
//  offsets that has to be recounted on every change.
//
//  Bounds are checked once, here, rather than at each of the forty places a
//  telemetry document writes a number. A write that does not fit sets the
//  writer bad and every later write is a no-op, so a caller checks ok() at the
//  end instead of after each field and cannot accidentally emit a truncated
//  document that decodes as something else.
//
//  Only the types this node emits are here. There is no reader: reading is
//  LxmfFormat.h's, and it reads a different, smaller set.
//
//  One trap is worth naming, because it has already cost this project a
//  release. msgpack distinguishes str from bin, Python distinguishes str from
//  bytes, and the two map onto each other — so which one a field wants is not
//  a matter of taste. An LXMF display name must be bin (it is read with
//  .decode()); a telemetry Information field must be str (it is written with
//  str() and rendered as-is). Emitting the other makes the value vanish or
//  render as b'...'. str() and bin() are separate calls here for that reason.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Rns {

class MsgPack {
public:
  MsgPack(uint8_t* out, size_t cap) : _p(out), _cap(cap) {}

  size_t size() const { return _n; }
  bool   ok() const { return _ok; }

  // Containers. The count is the number of members that follow — pairs for a
  // map, elements for an array — and it is the caller's to get right: a header
  // promising three pairs followed by two is not a document msgpack can read.
  MsgPack& map(size_t n)   { return container(n, 0x80, 0xDE); }
  MsgPack& array(size_t n) { return container(n, 0x90, 0xDC); }

  MsgPack& uint(uint64_t v) {
    if (v <= 0x7F)        return put((uint8_t)v);
    if (v <= 0xFF)        return put(0xCC).put((uint8_t)v);
    if (v <= 0xFFFF)      return put(0xCD).be(v, 2);
    if (v <= 0xFFFFFFFFu) return put(0xCE).be(v, 4);
    return put(0xCF).be(v, 8);
  }

  MsgPack& integer(int64_t v) {
    if (v >= 0) return uint((uint64_t)v);
    if (v >= -32)          return put((uint8_t)(0xE0 | (v + 32)));
    if (v >= -128)         return put(0xD0).put((uint8_t)(int8_t)v);
    if (v >= -32768)       return put(0xD1).be((uint64_t)(uint16_t)(int16_t)v, 2);
    if (v >= -2147483648LL) return put(0xD2).be((uint64_t)(uint32_t)(int32_t)v, 4);
    return put(0xD3).be((uint64_t)v, 8);
  }

  // Always float64. A node has no reason to save four bytes here and every
  // reason for a figure to survive the trip unrounded.
  MsgPack& real(double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, 8);
    return put(0xCB).be(bits, 8);
  }

  MsgPack& boolean(bool b) { return put(b ? 0xC3 : 0xC2); }
  MsgPack& nil()           { return put(0xC0); }

  // Text, which decodes to a Python str.
  MsgPack& str(const char* s, size_t n) {
    if (n <= 31)   put((uint8_t)(0xA0 | n));
    else if (n <= 0xFF)  put(0xD9).put((uint8_t)n);
    else                 put(0xDA).be(n, 2);
    return raw((const uint8_t*)s, n);
  }
  MsgPack& str(const char* s) { return str(s, s ? strlen(s) : 0); }

  // Bytes, which decode to Python bytes.
  MsgPack& bin(const uint8_t* p, size_t n) {
    if (n <= 0xFF) put(0xC4).put((uint8_t)n);
    else           put(0xC5).be(n, 2);
    return raw(p, n);
  }

  // A double narrowed to an integer field, held to that field's range rather
  // than allowed to wrap. Converting an out-of-range double to an integer type
  // is undefined in C++ and in practice wraps, which does not make a reading
  // wrong by a little: an accuracy estimate of 700 m becomes a confident
  // 44.6 m, which is worse than no reading at all.
  static int32_t clampI32(double v) {
    if (v <= -2147483648.0) return INT32_MIN;
    if (v >=  2147483647.0) return INT32_MAX;
    return (int32_t)v;
  }
  static uint32_t clampU32(double v) {
    if (v <= 0.0) return 0;
    if (v >= 4294967295.0) return UINT32_MAX;
    return (uint32_t)v;
  }
  static uint16_t clampU16(double v) {
    if (v <= 0.0) return 0;
    if (v >= 65535.0) return UINT16_MAX;
    return (uint16_t)v;
  }

  // A big-endian signed integer of a fixed width, as bytes rather than as a
  // number. Telemetry positions are packed this way — struct.pack("!i", …) on
  // the other side — so they arrive as bytes and not as msgpack integers.
  MsgPack& binI32(int32_t v)  { uint8_t b[4]; beBytes(b, (uint32_t)v, 4); return bin(b, 4); }
  MsgPack& binU32(uint32_t v) { uint8_t b[4]; beBytes(b, v, 4);           return bin(b, 4); }
  MsgPack& binU16(uint16_t v) { uint8_t b[2]; beBytes(b, v, 2);           return bin(b, 2); }

private:
  MsgPack& container(size_t n, uint8_t fix, uint8_t wide) {
    if (n <= 15) return put((uint8_t)(fix | n));
    return put(wide).be(n, 2);
  }
  MsgPack& put(uint8_t b) {
    if (!_ok || _n + 1 > _cap) { _ok = false; return *this; }
    _p[_n++] = b;
    return *this;
  }
  MsgPack& raw(const uint8_t* p, size_t n) {
    if (!_ok || _n + n > _cap) { _ok = false; return *this; }
    if (n) memcpy(_p + _n, p, n);
    _n += n;
    return *this;
  }
  MsgPack& be(uint64_t v, size_t width) {
    for (size_t i = 0; i < width; i++) put((uint8_t)(v >> (8 * (width - 1 - i))));
    return *this;
  }
  static void beBytes(uint8_t* out, uint32_t v, size_t width) {
    for (size_t i = 0; i < width; i++) out[i] = (uint8_t)(v >> (8 * (width - 1 - i)));
  }

  uint8_t* _p;
  size_t   _cap;
  size_t   _n = 0;
  bool     _ok = true;
};

} // namespace Rns
