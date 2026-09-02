// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiPower.cpp — the three-button question a long press asks
//
//  Sleep, Restart, Power off. The menu only records the answer; the display
//  task carries it out, because blanking is its property and a power-off
//  must not run inside an LVGL event callback that the shutdown will tear
//  out from under itself.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>

namespace {
volatile uint8_t sAction = 0;            // 0 none, 1 sleep, 2 restart, 3 off
lv_obj_t* sMenu = nullptr;

void act(lv_event_t* e) {
  sAction = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  if (sMenu) { lv_msgbox_close(sMenu); sMenu = nullptr; }
}
} // namespace

namespace Ui {

void openPowerMenu() {
  if (sMenu) return;                     // one at a time
  sMenu = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(sMenu, "Power");
  lv_msgbox_add_close_button(sMenu);
  struct { const char* label; uint8_t action; } rows[] = {
    { LV_SYMBOL_EYE_CLOSE "  Sleep",   1 },
    { LV_SYMBOL_REFRESH   "  Restart", 2 },
    { LV_SYMBOL_POWER     "  Power off", 3 },
  };
  lv_obj_t* body = lv_msgbox_get_content(sMenu);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  for (auto& r : rows) {
    lv_obj_t* btn = lv_button_create(body);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, r.label);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, act, LV_EVENT_CLICKED, (void*)(uintptr_t)r.action);
  }
}

uint8_t takePowerAction() {
  const uint8_t a = sAction;
  sAction = 0;
  return a;
}

} // namespace Ui
#endif // HAS_LVGL_UI
