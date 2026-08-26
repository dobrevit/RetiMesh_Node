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

#if HAS_DISPLAY
  #include <Wire.h>
  #include <Adafruit_SSD1306.h>
#endif

class Display {
public:
  // Returns false (and disables itself) when no panel answers on I2C.
  bool begin();
  bool present() const { return _ok; }

  // FreeRTOS entry point — created pinned to core 0 from main.cpp.
  static void displayTask(void* self);

private:
  void paint();

#if HAS_DISPLAY
  Adafruit_SSD1306 _oled{128, 64, &Wire, -1};
#endif
  bool _ok = false;
};

extern Display display;
