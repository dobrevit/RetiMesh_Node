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
//  Ui.h — what the shell's screens share
//
//  Internal to src/ui/lvgl: the public face of the GUI stays LvglUi.h. This
//  is the contract between the plumbing (UiShell.cpp) and the screens (Home,
//  Messages, Settings), so each screen is one file with one concern and none
//  of them owns a display buffer or a keyboard of its own.
//
//  Navigation is a stack with Home at the bottom: push() slides a screen in
//  and remembers where it came from, back() slides it away and deletes it.
//  The status bar, the keyboard and the toasts live on LVGL's top layer, so
//  they ride above every screen without any screen knowing.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_LVGL_UI

#include <lvgl.h>

class TftPanel;

namespace Ui {

// --- plumbing (UiShell.cpp) -------------------------------------------------

bool shellInit(TftPanel& panel);         // lvgl, buffers, touch, bar, keyboard
uint32_t shellLoop();                    // lv_timer_handler + the bar's second
bool consumeTouch();                     // a finger since last asked (sticky)
void swallowTouch();                     // ignore the current contact until it lifts

// The navigation stack. push() loads `screen` and keeps the current one to
// come back to; back() returns and deletes the screen it leaves. Home never
// leaves the bottom of the stack.
void push(lv_obj_t* screen);
void back();
bool atRoot();

// A new screen with the standard chrome: cleared below the status bar, and —
// when `title` is given — a header row carrying a back arrow and the title.
// Returns the content container the caller fills (flex column, scrollable).
lv_obj_t* newScreen(const char* title);

// A transient result line, top centre, above everything.
void toast(const char* text);

// A textarea wired to the shared keyboard: focus summons it (numeric mode for
// number fields), defocus dismisses it. The parent form scrolls the field
// clear of the keys.
lv_obj_t* textarea(lv_obj_t* parent, const char* placeholder,
                   bool oneLine, bool numeric);

// --- screens ----------------------------------------------------------------

void openHome();                         // builds and loads the root screen
void openMessages();                     // push()es the messages screen
void openSettings();                     // push()es the category list
void openWifiJoin();                     // scan, pick, key if locked, save on success
void openAbout();                        // a dialog: version, addresses
void openPowerMenu();                    // sleep / restart / power off
uint8_t takePowerAction();               // 0 none, 1 sleep, 2 restart, 3 off
void setRotation(uint8_t quarterTurns);  // the accelerometer's verdict
#if HAS_GPS
void openGps();                          // push()es the receiver's readings
#endif

} // namespace Ui

#endif // HAS_LVGL_UI
