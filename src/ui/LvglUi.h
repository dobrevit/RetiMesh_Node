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
//  LvglUi.h — the touch shell on colour boards
//
//  Three tabs, mirroring the portal's three main pages: Status, Messages,
//  Settings. The milestone this exists for is doing on the glass what those
//  web pages do — read the node, read the messages, change the settings —
//  with a real on-screen keyboard where the web page had an input field.
//
//  Ownership rules, which are the page stack's rules continued:
//
//    Every LVGL call happens on the display task. The library is
//    single-threaded by design and so is our use of it: begin() and loop()
//    are display-task-only, and the data the widgets show arrives through
//    the same snapshot/accessor pattern the mono pages read.
//
//    Settings changes go through the console's own funnel
//    (SettingsFields::applyKey), because that is where validation and
//    apply-live behaviour already live. The GUI is another caller of the one
//    rule, not a second implementation of it.
//
//    Sending a message uses the transport's reply queue, which hands the
//    text to the Reticulum task — the GUI never touches the library.
//
//  The module compiles to nothing on boards without HAS_LVGL_UI; the mono
//  page stack is untouched everywhere it runs today.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_LVGL_UI

class TftPanel;

namespace LvglUi {

// Bring the toolkit up on this panel: display, buffers, touch input, theme,
// and the three tabs. Display-task context (or setup(), before the task
// exists). Returns false when LVGL could not be given its buffers.
bool begin(TftPanel& panel);

// One pass: feed the tick, run LVGL's timers, refresh whichever tab is
// showing if its refresh period has come. Returns the delay in ms LVGL asks
// for until it wants running again (bounded by the caller's own poll rate).
uint32_t loop();

// The physical buttons, mapped onto the shell: step between tabs. The touch
// layer talks to LVGL directly as a pointer device; the buttons remain useful
// with gloves on.
void stepTab(int8_t dir);

// Blank/unblank hooks so the panel sleep logic stays where it is.
void onBlank(bool on);

// Whether the glass has been touched since last asked — the display's
// activity timer eats this, since the shell owns the touch layer.
bool touchActive();

// Ignore the contact currently on the glass until it lifts — called when a
// tap wakes the panel, so waking is all that tap does.
void swallowTouch();

// The long press's question and its answer, and the accelerometer's say.
void openPowerMenu();
uint8_t takePowerAction();               // 0 none, 1 sleep, 2 restart, 3 off
void setRotation(uint8_t quarterTurns);

} // namespace LvglUi

#else

class TftPanel;
namespace LvglUi {
inline bool begin(TftPanel&) { return false; }
inline uint32_t loop() { return 1000; }
inline void stepTab(int8_t) {}
inline void onBlank(bool) {}
inline bool touchActive() { return false; }
inline void swallowTouch() {}
inline void openPowerMenu() {}
inline uint8_t takePowerAction() { return 0; }
inline void setRotation(uint8_t) {}
} // namespace LvglUi

#endif // HAS_LVGL_UI
