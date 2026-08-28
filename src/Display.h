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
#include "Power.h"

#if HAS_DISPLAY
  #include <Wire.h>
  #include <Adafruit_SSD1306.h>
#endif

class Display {
public:
  // Probes the I2C bus for a panel (ACK at OLED_ADDR, then the alternate
  // address) before touching the driver. Returns false, and the module
  // stays inert, when nothing answers — Adafruit's begin() alone would
  // happily "succeed" against an empty bus.
  bool begin();
  bool present() const { return _ok; }
  uint8_t address() const { return _addr; }

  // FreeRTOS entry point — created pinned to core 0 from main.cpp.
  static void displayTask(void* self);

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
  uint32_t _pressedAtMs = 0;             // 0 = not pressed
  bool     _longFired = false;

#if HAS_DISPLAY
  Adafruit_SSD1306 _oled{DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, PIN_OLED_RST};
#endif
  static bool ack(uint8_t addr);         // true if a device ACKs at addr
  bool    _ok   = false;
  uint8_t _addr = 0;
};

extern Display display;
