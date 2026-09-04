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
#include "LxmfInbox.h"
#include "OtaUpdate.h"
#include "Power.h"

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

bool activateFocused() { return Ui::activateFocused(); }

void onBlank(bool on) {
  if (!on) lv_obj_invalidate(lv_screen_active());
}

bool touchActive() { return Ui::consumeTouch(); }

PanelAction restTick(uint32_t nowMs, uint32_t& lastActivityMs,
                     bool blank, bool canBlank) {
  // The shell owns the touch layer, so the activity timer has to ask it
  // about fingers — the buttons alone left the panel blanking under an
  // operator mid-navigation.
  if (touchActive()) lastActivityMs = nowMs;

  bool claimed = false;
  // A fresh message interrupts everything: wake the glass and put the
  // sender on it — the spec's full-screen alert, not a corner toast,
  // because a field device is glanced at, not watched.
  Rns::InboxRecord nr;
  if (Rns::Inbox::takeNotice(nr)) {
    lastActivityMs = nowMs;
    showIdle(false);
    claimed = true;
    char text[81];
    const size_t tn = nr.textLen < sizeof(text) - 1 ? nr.textLen : sizeof(text) - 1;
    memcpy(text, nr.text, tn); text[tn] = 0;
    showIncoming(nr.from, text);
  }
  {
    // An update owns the glass for its whole journey — receive, staging
    // and install alike, from the same Progress record the portal
    // serves, so the two can never tell different stories. On the edge
    // back to idle a failure takes the glass with its own message.
    static bool wasBusy = false;
    const Ota::Progress op = Ota::progress();
    const bool busy = op.stage == Ota::Stage::Receiving ||
                      op.stage == Ota::Stage::Staged ||
                      op.stage == Ota::Stage::Installing;
    if (busy) {
      lastActivityMs = nowMs;
      showIdle(false);
      claimed = true;
      showFirmware(Ota::describe(op.stage), op.received, op.expected);
      wasBusy = true;
    } else if (wasBusy) {
      wasBusy = false;
      hideFirmware();
      if (op.stage == Ota::Stage::Failed)
        showIncoming(nullptr, op.message[0]
            ? op.message
            : "Firmware update failed — still on the old version.");
    }
  }
  if (claimed) return PanelAction::Wake;

  // The shell rests in two stages: first the spec's idle clock — the
  // screen this device spends its life on, radio still listening — and
  // the true blank only after four quiet timeouts, because the
  // backlight is still the real money.
  const uint32_t quiet = nowMs - lastActivityMs;
  if (!blank) {
    if (canBlank && quiet > Power::displaySleepMs() * 4) {
      showIdle(false);
      return PanelAction::Sleep;
    }
    showIdle(quiet > Power::displaySleepMs());
  }
  return PanelAction::None;
}

void swallowTouch() { Ui::swallowTouch(); }

void openPowerMenu() { Ui::openPowerMenu(); }
void showIdle(bool on) { Ui::showIdle(on); }
bool idleShowing() { return Ui::idleShowing(); }
void showIncoming(const uint8_t* f, const char* t) { Ui::showIncoming(f, t); }
void showFirmware(const char* st, uint32_t w, uint32_t t) { Ui::showFirmware(st, w, t); }
void hideFirmware() { Ui::hideFirmware(); }
uint8_t takePowerAction() { return Ui::takePowerAction(); }
void setRotation(uint8_t q) { Ui::setRotation(q); }

} // namespace LvglUi

#endif // HAS_LVGL_UI
