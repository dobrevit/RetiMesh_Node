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
//  RefreshPolicy.h — whether a frame that has just been drawn is worth the
//  cost of putting on the glass
//
//  Every page in this firmware was written against an OLED, where pushing a
//  frame is a kilobyte over I2C and nobody minds it happening twice a second
//  whether or not anything changed. E-paper is the opposite: an update takes
//  hundreds of milliseconds, flashes the panel while it happens, and wears the
//  film. On a panel like that "redraw at 2 fps" is not a slow version of the
//  same thing, it is a broken node.
//
//  So the drawing and the showing come apart. A page draws whenever it likes,
//  into memory, costing nothing; this decides whether what was drawn differs
//  from what the glass already shows, and whether enough time has passed for
//  the panel to afford another update. Both answers come from the panel's own
//  descriptor (DisplayLayout), so the same page code is right on either.
//
//  Pure — no Arduino, no panel, no clock of its own — so the decisions are
//  unit-tested on the host rather than guessed at on a bench.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

class RefreshPolicy {
public:
  enum class Action : uint8_t {
    Skip,      // the glass already shows this, or the panel cannot afford it yet
    Partial,   // push the difference: the ordinary update
    Full,      // a whole-panel refresh, which e-paper needs periodically
  };

  // `minIntervalMs` is the shortest gap between updates the panel can afford.
  // `fullEveryMs` is how often it wants a whole-panel refresh regardless — 0
  // on a panel that never needs one, which is every panel that is not e-paper.
  // fullEveryUpdates is counted in updates and not in time because that is
  // how the ghosting accrues: it is left behind by each partial update, so a
  // panel that has sat unchanged for an hour owes nothing and one that has
  // been pressed through twenty pages in a minute owes several. Good Display,
  // who make this family of panels, suggest one full refresh per five partial.
  RefreshPolicy(uint32_t minIntervalMs = 0, uint16_t fullEveryUpdates = 0)
    : _minIntervalMs(minIntervalMs), _fullEvery(fullEveryUpdates) {}

  // The gap changes with what the panel is showing: a page somebody has just
  // turned to is worth keeping current, the one a node rests on for days is
  // not (Display.cpp).
  void interval(uint32_t ms) { _minIntervalMs = ms; }

  // `frame` identifies what was just drawn — a hash of the buffer. Equal
  // frames mean the glass is already right.
  Action decide(uint32_t nowMs, uint32_t frame) {
    // The first frame after forget() (or after boot) always goes out: the
    // panel holds whatever the last firmware left in it, and an identical
    // hash proves nothing about glass we have not written to. On a panel that
    // does whole-panel refreshes, that is exactly when it wants one — what is
    // on the glass is not ours and may be a ghost of it forever otherwise.
    const bool unknown  = !_shown;
    const bool changed  = unknown || frame != _frame;
    const bool fullDue  = _fullEvery && (unknown || _sinceFull >= _fullEvery);
    if (!changed && !fullDue) return Action::Skip;
    // Deliberately not "flush the moment it changes": a value that flickers
    // between two states — a signal reading, a countdown — would otherwise
    // update the panel as fast as it changes. The gap is what the panel can
    // afford, so it applies to the change as much as to the repeat.
    //
    // Unless somebody asked. The gap is there to keep data churn off the
    // panel, not to make a person wait: a button press that turns the page
    // and then shows nothing for five seconds reads as a node that did not
    // notice the press.
    if (_shown && !_urgent && (uint32_t)(nowMs - _last) < _minIntervalMs) return Action::Skip;

    _frame  = frame;
    _shown  = true;
    _urgent = false;
    _last   = nowMs;
    if (fullDue) { _sinceFull = 0; return Action::Full; }
    _sinceFull++;
    return Action::Partial;
  }

  // The next frame goes out whatever it holds: the panel has been powered
  // down, or is about to show something a page did not draw (a boot notice),
  // so what we believe is on the glass is no longer true.
  void forget() { _shown = false; }

  // The next differing frame goes out without waiting out the panel's gap.
  // For a change a person asked for — turning the page — where the gap is
  // the difference between a panel that responds and one that appears stuck.
  // Not forget(): the glass is not unknown, it shows the page before this
  // one, so this stays a quiet partial update rather than a full flash.
  void urgent() { _urgent = true; }

  // For a panel that cannot show its buffer: every frame counts as new.
  static uint32_t alwaysNew(uint32_t counter) { return counter; }

  // FNV-1a over the frame buffer. Cheap enough to run on every draw — a
  // 128x64 panel is a kilobyte — and what makes "has anything changed?"
  // answerable without keeping a second copy of the frame.
  static uint32_t hash(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
  }

private:
  uint32_t _minIntervalMs;
  uint16_t _fullEvery;             // partial updates taken before a full one
  uint16_t _sinceFull = 0;
  uint32_t _frame    = 0;
  uint32_t _last     = 0;
  bool     _shown    = false;      // anything known to be on the glass yet
  bool     _urgent   = false;      // somebody is waiting on the next frame
};
