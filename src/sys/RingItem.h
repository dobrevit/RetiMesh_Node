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
//  RingItem.h — hold a FreeRTOS ring-buffer item, and give it back on every exit
//
//  The same problem Lock.h solves, on the other resource this firmware checks
//  out and must hand back. An item received from a ring is borrowed space: the
//  ring cannot reuse it until vRingbufferReturnItem is called, and a return
//  written out by hand at the end of a loop body is correct only while nothing
//  leaves that body another way.
//
//  Something does. The drain loops call into microReticulum — handle_incoming
//  for the radio, incoming() for a Wi-Fi client — and those allocate. A
//  bad_alloc unwinds straight past the return, and since the loop that drives
//  them runs under Diag::guard the node does not restart: it carries on,
//  minus one frame's worth of ring, for ever. Enough of those and the ring is
//  full, the radio task can no longer post what it received, and the node goes
//  deaf while its interface list still reports rising counters.
//
//  That is the failure this firmware is least able to see, so the return is
//  made structural rather than remembered.
// ============================================================================
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

namespace Sys {

class RingItem {
public:
  // Receives nothing itself: the caller has the ring, the timeout and the
  // size out-parameter to hand it, and this only owns what came back.
  RingItem(RingbufHandle_t ring, void* item) : _ring(ring), _item(item) {}
  ~RingItem() { if (_ring && _item) vRingbufferReturnItem(_ring, _item); }

  explicit operator bool() const { return _item != nullptr; }
  uint8_t* bytes() const { return static_cast<uint8_t*>(_item); }

  RingItem(const RingItem&) = delete;
  RingItem& operator=(const RingItem&) = delete;

private:
  RingbufHandle_t _ring;
  void*           _item;
};

} // namespace Sys
