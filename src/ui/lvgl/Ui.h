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

// Set a label only when the text changed — LVGL reallocates and repaints on
// every set — and only while the label still exists. Stated once; three
// screens had grown three variants with three different guards.
void setLabel(lv_obj_t* label, const char* text);

// A distance in the one style every screen prints it.
void formatKm(char* out, size_t n, double km);

// Milliseconds-since into the age wording, through ageTextS — the ms-to-s
// conversion had started spreading again.
void ageTextMs(uint32_t sinceMs, char* out, size_t n);

// Seconds into the s/m/h wording every age on the glass uses. One unit on
// purpose: two same-named helpers taking seconds and milliseconds were one
// copy-paste away from ages wrong by a thousand.
void ageTextS(uint32_t seconds, char* out, size_t n);

// The two-second hold the system's irreversible actions demand — stated
// once, because a timing tweak that reaches one red control and not the
// other is the worst drift this firmware could grow.
void onHeld2s(lv_obj_t* btn, void (*fire)(void*), void* userData);

// A hash grouped in fours across two lines, to be verified out loud.
void groupedHash(const char* hex, char* out, size_t n);

// A textarea wired to the shared keyboard: focus summons it (numeric mode for
// number fields), defocus dismisses it. The parent form scrolls the field
// clear of the keys.
lv_obj_t* textarea(lv_obj_t* parent, const char* placeholder,
                   bool oneLine, bool numeric);

// --- screens ----------------------------------------------------------------

void openHome();                         // builds and loads the root screen
void openMessages();                     // push()es the messages screen
void openThread(const uint8_t from[16]);  // one conversation, directly
// The rule for naming a peer, stated once: the live announce table first,
// the persistent name memory second, the shortened hash last.
void peerLabelHex(const char* hashHex, char* out, size_t n);
void peerLabel(const uint8_t hash[16], char* out, size_t n);
void openDestinations();                 // the mesh: peers by hops and freshness
void openSettings();                     // push()es the category list
void openWifiJoin();                     // scan, pick, key if locked, save on success
void openAbout();                        // a dialog: version, addresses
void openIdentity();                     // who this node is, and its QR
void openPowerMenu();                    // sleep / restart / power off
void showIdle(bool on);                  // the low-draw clock the panel rests on
bool idleShowing();
void resetIdle();                        // drops the idle panel so it rebuilds
void retheme();                          // applies the settings' palette, rebuilds
void showIncoming(const uint8_t from[16], const char* text);  // full-screen interrupt
void showFirmware(const char* stage, uint32_t written, uint32_t total);
void hideFirmware();
void openBearing(const char* peer, const char* hashHex);  // the live dial
void openPlot();                         // rings and a crosshair, no cartography
uint8_t takePowerAction();               // 0 none, 1 sleep, 2 restart, 3 off
void setRotation(uint8_t quarterTurns);  // the accelerometer's verdict
#if HAS_GPS
void openGps();                          // push()es the receiver's readings
#endif

} // namespace Ui

#endif // HAS_LVGL_UI
