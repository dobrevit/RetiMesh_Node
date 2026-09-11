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
//  UartTxQueue.h — frames waiting for a wire, and what happens when too many
//
//  The bounded half of the UART byte-stream driver, kept here with no Arduino
//  include so the two things the roadmap actually asks to be proved — that the
//  queue is counted and that it cannot grow without bound — are proved on a
//  host rather than watched on a bench.
//
//  It stores whole frames, not bytes. A UART link carries HDLC frames
//  (HDLC.h), and half a frame on the wire is not a partial delivery but a
//  corruption the far end has to resynchronise out of. Bounding by bytes would
//  make that possible; bounding by frames means the thing dropped is always a
//  thing that was never started.
//
//  It allocates nothing
//  --------------------
//  The arena is the caller's, handed over by `attach`. That is not fastidious-
//  ness: a disabled subsystem must cost no DRAM (project-brief), and a link
//  that is switched off has no business holding kilobytes on a board with ten
//  to spare. The driver allocates on enable and frees on disable, the shape
//  Buzzer uses for its I2S task, and this class is the part that has no
//  opinion about when that happens.
//
//  Dropping the newest, not the oldest
//  -----------------------------------
//  A full queue refuses the incoming frame and says so. The alternative —
//  evicting the oldest to make room — is wrong for this link for a reason
//  worth stating once: the frames already queued are the ones a peer is
//  waiting on, and a sender that discards them to admit newer traffic turns a
//  slow link into a lossy one at exactly the moment the far end is least able
//  to tell the difference. Refusing is backpressure the caller can see; the
//  drop counter is how it sees it.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace Uart {

// What the link has done, in the shape every surface in this project reports:
// a count that only rises, and a high-water mark beside the live figure so a
// reading taken now can still describe a burst that happened between samples.
struct TxCounters {
  uint32_t framesQueued  = 0;   // accepted by push()
  uint32_t framesSent    = 0;   // handed to the wire by pop()
  uint32_t framesDropped = 0;   // refused: no room
  uint32_t bytesQueued   = 0;
  uint32_t bytesSent     = 0;
  uint16_t depth         = 0;   // frames waiting right now
  uint16_t depthHighWater = 0;  // the most that have ever waited
};

class TxQueue {
public:
  // The arena and the frame table are both the caller's. `slots` bounds the
  // number of frames and `bytes` bounds their total size; a push fails when
  // either is exhausted, because either one running out is the queue being
  // full and the caller does not care which.
  struct Slot { uint32_t off; uint16_t len; };

  void attach(uint8_t* arena, size_t bytes, Slot* slots, size_t count) {
    _arena = arena; _cap = bytes;
    _slots = slots; _slotCap = count;
    _head = _tail = _depth = 0;
    _used = 0;
  }

  // Hands the memory back. The totals survive deliberately: a link switched off
  // and on again has not un-dropped the frames it dropped, and a surface that
  // showed them should not start lying because somebody toggled it.
  //
  // `depth` is not one of the totals and does go to zero. It is a gauge — what
  // is waiting *now* — and a detached queue is holding nothing, so leaving it
  // at its last value made `counters().depth` report frames that no longer
  // exist, for ever, on a link that was switched off. The rest of this class
  // keeps `_c.depth` in step with `_depth` on every push and pop; this was the
  // one path that did not.
  void detach() {
    _arena = nullptr; _slots = nullptr;
    _cap = _slotCap = 0;
    _head = _tail = _depth = 0;
    _used = 0;
    _c.depth = 0;
  }

  // Capacity is part of being attached, not merely the pointers: the ring
  // arithmetic takes a modulus by _cap, so a zero-sized arena would divide by
  // zero rather than simply refuse every push.
  bool attached() const {
    return _arena != nullptr && _slots != nullptr && _cap > 0 && _slotCap > 0;
  }

  // True when the frame was taken. False means dropped, and counted.
  bool push(const uint8_t* frame, size_t len) {
    if (!attached() || len == 0 || len > 0xFFFF) { _c.framesDropped++; return false; }
    if (_depth >= _slotCap || _used + len > _cap) { _c.framesDropped++; return false; }
    // Contiguous or wrapped, the arena is written through a helper so the two
    // cases cannot drift: a wrapped frame that copied only its first half was
    // the bug this shape exists to make impossible.
    const uint32_t off = (uint32_t)((_headByte()) % _cap);
    writeWrapped(off, frame, len);
    _slots[_tail] = Slot{off, (uint16_t)len};
    _tail = (uint16_t)((_tail + 1) % _slotCap);
    _depth++;
    _used += len;
    _c.framesQueued++;
    _c.bytesQueued += (uint32_t)len;
    _c.depth = _depth;
    if (_depth > _c.depthHighWater) _c.depthHighWater = _depth;
    return true;
  }

  // The oldest frame, copied out. False when there is nothing waiting.
  bool pop(uint8_t* out, size_t outCap, size_t& len) {
    if (!attached() || _depth == 0) return false;
    const Slot s = _slots[_head];
    if (s.len > outCap) {
      // The caller's buffer cannot hold it. Dropped rather than truncated:
      // half an HDLC frame on the wire is a corruption the far end has to
      // resynchronise out of, which is worse than the frame never arriving.
      consume(s);
      _c.framesDropped++;
      return false;
    }
    readWrapped(s.off, out, s.len);
    len = s.len;
    consume(s);
    _c.framesSent++;
    _c.bytesSent += s.len;
    return true;
  }

  uint16_t depth() const { return _depth; }
  size_t   bytesUsed() const { return _used; }
  bool     empty() const { return _depth == 0; }
  const TxCounters& counters() const { return _c; }

  // Only for a surface that wants to show the live figures beside the totals.
  void refresh() { _c.depth = _depth; }

private:
  uint32_t _headByte() const {
    // Where the next frame starts: the tail of the last one, or the head when
    // the queue is empty (which lets a drained queue reuse the whole arena
    // rather than creeping forward until it wraps for no reason).
    if (_depth == 0) return 0;
    const Slot last = _slots[(_tail + _slotCap - 1) % _slotCap];
    return (last.off + last.len) % _cap;
  }

  void writeWrapped(uint32_t off, const uint8_t* src, size_t len) {
    const size_t first = (off + len <= _cap) ? len : (_cap - off);
    memcpy(_arena + off, src, first);
    if (first < len) memcpy(_arena, src + first, len - first);
  }

  void readWrapped(uint32_t off, uint8_t* dst, size_t len) const {
    const size_t first = (off + len <= _cap) ? len : (_cap - off);
    memcpy(dst, _arena + off, first);
    if (first < len) memcpy(dst + first, _arena, len - first);
  }

  void consume(const Slot& s) {
    _head = (uint16_t)((_head + 1) % _slotCap);
    _depth--;
    _used -= s.len;
    _c.depth = _depth;
  }

  uint8_t* _arena = nullptr;
  size_t   _cap = 0;
  Slot*    _slots = nullptr;
  size_t   _slotCap = 0;
  uint16_t _head = 0, _tail = 0, _depth = 0;
  size_t   _used = 0;
  TxCounters _c;
};

} // namespace Uart
