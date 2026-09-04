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
//  Display.h — SSD1306 OLED status page
//
//  Initialised early in setup() so the panel stops showing whatever the
//  previous firmware left in its RAM, then repainted from a low-priority
//  core-0 task. Reads only g_stats and the Wi-Fi manager — never touches
//  the radio — so it can never delay packet handling.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"
#include "DisplayLayout.h"
#include "Panel.h"
#include "RefreshPolicy.h"
#include "Power.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_OLED
  #include "OledPanel.h"
#elif HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_EINK
  #include "EinkPanel.h"
#elif HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_TFT
  #include "TftPanel.h"
#endif

class Display {
public:
  // Asks the panel to find itself and start (OledPanel::begin for an
  // SSD1306: the switched rail, the stuck-bus recovery, the two-address
  // probe). Returns false, and the module stays inert, when there is no
  // panel — which is also what a board with no screen at all gets.
  bool begin();
  bool present() const { return _ok; }

  // What the colour controller said it is, where it could be asked at all.
  // Zero everywhere else. See TftPanel::controllerId().
  uint32_t controllerId() const {
#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_TFT
    return _panelImpl.controllerId();
#else
    return 0;
#endif
  }

  // FreeRTOS entry point — created pinned to core 0 from main.cpp.
  static void displayTask(void* self);

  // A line of text painted straight to the panel, for the parts of start-up
  // that take long enough for a person to wonder whether the node has hung.
  // Moving the Reticulum store is seconds of filesystem work before any task
  // exists, and a panel that says nothing through it reads as a node that has
  // died during boot — which is exactly when somebody reaches for the power.
  // Callable before the display task exists, because that is the whole point:
  // it paints synchronously rather than asking to be painted later.
  void notice(const char* title, const char* detail = nullptr);

private:
  void paint();
  void paintStatus();
  void paintNeighbors();
  void paintRadio();
  void paintNetwork();
#if !DISPLAY_COMPACT
  void paintQr();
#endif
#if HAS_GPS
  void paintGps();
#endif
  void paintTransport();
  void pollButton();
  void header(const char* title);        // page name + the cell
  void meter(uint8_t row, const char* label, const char* value, uint8_t pct);


  // The GNSS page exists only where there is a receiver to read, and the QR
  // page only where a camera could read the result. On a 0.49" panel the
  // module pitch is 0.17 mm and a version-3 symbol is 5.4 mm across — below
  // what a phone lens resolves and inside its near-focus blur, so it scans as
  // a single bright blob however it is drawn. A page that cannot work costs a
  // button press to reach and another to leave, so it is not in the cycle.
#if HAS_GPS
  enum Page : uint8_t { STATUS = 0, NEIGHBORS, TRANSPORT, RADIO, NETWORK, GPS,
  #if !DISPLAY_COMPACT
    QR,
  #endif
    PAGE_COUNT };
#else
  enum Page : uint8_t { STATUS = 0, NEIGHBORS, TRANSPORT, RADIO, NETWORK,
  #if !DISPLAY_COMPACT
    QR,
  #endif
    PAGE_COUNT };
#endif
  // Typed, so paint()'s switch can list every page and let the compiler
  // object when one is added without being drawn (-Werror=switch).
  Page stepPage(Page p, int8_t dir) const;   // the next page that means something, either way
  bool skipPage(Page p) const;               // the rule, stated once
  Page     _page = STATUS;
  uint8_t  _chargeSweep = 0;             // animates the battery fill while charging
  // Sampled once per frame in paint(). Reading the PMU costs four I2C
  // transactions on a bus shared with the panel, and two reads in one frame
  // could disagree — the header icon and the text would then contradict.
  Power::Battery _bat{};
  bool     _blank = false;
  uint32_t _pageChangedMs = 0;
  uint32_t _lastActivityMs = 0;          // last button press (boot counts)
  void setBlank(bool blank);             // DISPLAYOFF/ON on the panel
  void advancePage(bool forward);
  void longPressAction();               // both buttons' long press, stated once        // what any short press does
  // One press grammar for every input, written once (a review found it
  // hand-copied three times, already drifting): press is registered on the
  // edge, a hold past BUTTON_LONG_MS fires Long exactly once and latches so
  // the release stays silent, and a release counts as Short only after 30 ms
  // of press and two consecutive "up" polls — the touch layer answers only
  // while touched, and one dropped report mid-hold must not read as a tap.
  struct PressTracker {
    enum class Event : uint8_t { None, Press, Short, Long };
    uint32_t pressedAt = 0;              // 0 = not pressed
    bool     longFired = false;
    uint8_t  upStreak  = 0;
    Event update(bool down, uint32_t now) {
      if (down) {
        upStreak = 0;
        if (pressedAt == 0) { pressedAt = now; longFired = false; return Event::Press; }
        if (!longFired && now - pressedAt >= BUTTON_LONG_MS) { longFired = true; return Event::Long; }
        return Event::None;
      }
      if (pressedAt == 0) return Event::None;
      if (++upStreak < 2) return Event::None;
      const bool tapped = !longFired && now - pressedAt >= 30;
      pressedAt = 0; upStreak = 0;
      return tapped ? Event::Short : Event::None;
    }
  };
  PressTracker _btn;
#if HAS_TOUCH
  PressTracker _touch;                   // separate state: a finger and a thumb
  uint32_t _lastTouchPollMs = 0;         // the bus is asked slowly while idle
  void pollTouch();
#endif
#if HAS_BUTTON2
  PressTracker _btn2;
  void pollButton2();
#endif

#if HAS_DISPLAY
  #if DISPLAY_KIND == DISPLAY_KIND_OLED
  OledPanel     _panelImpl;
  #elif DISPLAY_KIND == DISPLAY_KIND_EINK
  EinkPanel     _panelImpl;
  #elif DISPLAY_KIND == DISPLAY_KIND_TFT
  TftPanel      _panelImpl;
  #endif
  Panel*        _panel = nullptr;        // the glass; null until begin() succeeds
  Adafruit_GFX* _gfx   = nullptr;        // what the pages draw on, from the panel
  // Drawing is free and showing is not, so they are decided separately: pages
  // draw every pass, and this says whether the result is worth putting on the
  // glass (RefreshPolicy.h). On an OLED it saves a kilobyte of I2C per
  // unchanged frame; on e-paper it is the difference between a usable panel
  // and one that flashes continuously.
  RefreshPolicy _refresh{DisplayLayout::active().refreshMs,
                         DisplayLayout::active().fullEveryUpdates};
  // The cadence the node is resting at, or the one it uses while somebody is
  // on a page they turned to. Kept here because the task loop draws at it and
  // the policy shows at it, and the two must not disagree.
  uint32_t      cadence() const {
    const DisplayLayout::Layout l = DisplayLayout::active();
    return _page == STATUS ? l.refreshMs : l.activeRefreshMs;
  }
  uint32_t      _frameSeq = 0;           // stands in for the hash on a panel with no readable buffer
  uint32_t      _lastPaintMs = 0;
  bool          _paintDue = true;        // boot, a page change, waking: draw without waiting for the cadence
#endif
  bool    _ok   = false;
};

extern Display display;
