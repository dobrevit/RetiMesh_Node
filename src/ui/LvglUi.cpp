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
//  LvglUi.cpp — the public face of the shell; the shell lives in lvgl/
//
//  What used to be one file is now the shape the UI/UX asked for: UiShell
//  owns the plumbing and the mobile chrome — the status bar, the keyboard,
//  the navigation stack — and one file per screen owns its concern (UiHome,
//  UiMessages, UiSettings, UiGps). This facade keeps the display module's
//  view of the GUI unchanged.
// ============================================================================
#include "LvglUi.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "lvgl/Ui.h"
#include "TftPanel.h"

namespace LvglUi {

bool begin(TftPanel& panel) {
  if (!Ui::shellInit(panel)) return false;
  Ui::openHome();
  return true;
}

uint32_t loop() { return Ui::shellLoop(); }

// The physical buttons: forward opens the first shortcut from home and walks
// back anywhere else, back always walks back — the whole shell is drivable
// with gloves on.
void stepTab(int8_t dir) {
  if (dir < 0) { Ui::back(); return; }
  if (Ui::atRoot()) Ui::openMessages();
  else Ui::back();
}

void onBlank(bool on) {
  if (!on) lv_obj_invalidate(lv_screen_active());
}

bool touchActive() { return Ui::consumeTouch(); }

void swallowTouch() { Ui::swallowTouch(); }

void openPowerMenu() { Ui::openPowerMenu(); }
void showIdle(bool on) { Ui::showIdle(on); }
bool idleShowing() { return Ui::idleShowing(); }
void showIncoming(const char* s, const char* t) { Ui::showIncoming(s, t); }
uint8_t takePowerAction() { return Ui::takePowerAction(); }
void setRotation(uint8_t q) { Ui::setRotation(q); }

} // namespace LvglUi

#endif // HAS_LVGL_UI
