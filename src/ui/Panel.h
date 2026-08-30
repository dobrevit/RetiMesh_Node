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
//  Panel.h — the glass, whatever it is made of
//
//  Every page in this firmware draws with Adafruit_GFX calls, which is the
//  part that does not care what the panel is: setCursor, print, drawBitmap
//  mean the same thing on an OLED, an e-paper film and a TFT. What differs is
//  everything around the drawing — how the panel is found and started, what
//  "show it now" costs, whether it can be switched off, and which value is
//  ink. That is what this interface holds, and it is all a new panel has to
//  bring.
//
//  Deliberately not a drawing API of its own. A wrapper around GFX would be a
//  second way to say the same thing, and every page would have to be rewritten
//  against it for no gain; pages keep the GFX they were written with and the
//  panel is asked only the questions GFX cannot answer.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "DisplayLayout.h"

// A board with no screen does not build the drawing library at all — its env
// excludes it, so that any use of a panel outside a HAS_DISPLAY guard fails to
// compile rather than quietly pulling the OLED driver into a headless build.
// The interface is part of that: nothing headless has a panel to describe.
#if HAS_DISPLAY

#include <Adafruit_GFX.h>

class Panel {
public:
  virtual ~Panel() {}

  // Find the panel and start it. False leaves the display module inert — the
  // board runs headless rather than talking to glass that is not there.
  virtual bool begin() = 0;
  virtual bool present() const = 0;

  // What pages draw on. Valid only once begin() has returned true.
  virtual Adafruit_GFX& gfx() = 0;

  // Erase the frame in memory. Costs nothing on any panel; it is flush()
  // that reaches the glass.
  virtual void clear() = 0;

  // Put the frame on the glass. `full` asks for a whole-panel refresh, which
  // e-paper needs periodically to clear its ghosting and which a panel with
  // nothing to clear may ignore. This is the call that is allowed to be slow.
  virtual void flush(bool full) = 0;

  // Power the panel down and up. A panel that cannot do it says nothing and
  // stays lit; the caller has already stopped drawing to it either way.
  virtual void blank(bool on) = 0;

  // Whether blanking saves anything. False on a panel that holds its image
  // without power — blanking that costs nothing to skip, and skipping it is
  // what keeps a shelf node readable. The display's sleep timer asks before
  // it stops painting, because on such a panel stopping is all it would do.
  virtual bool blanks() const = 0;

  // The frame as bytes, so the caller can tell whether it differs from what
  // was last shown and skip an update that would change nothing. A panel that
  // keeps no buffer we can see returns nullptr, and then every frame is
  // treated as new — correct, just not free.
  virtual const uint8_t* frame(size_t& len) const = 0;

  // Monochrome panels disagree about which value is dark: on an SSD1306 ink
  // is 1, on e-paper drivers it is usually 0. Pages ask rather than assume.
  virtual uint16_t ink() const = 0;
  virtual uint16_t paper() const = 0;

  // The geometry and refresh costs of this panel, for the pages and for the
  // refresh policy. Compile-time today (a board knows its own screen), an
  // accessor because the panel is the honest owner of the answer.
  virtual DisplayLayout::Layout layout() const { return DisplayLayout::active(); }
};

#endif // HAS_DISPLAY
