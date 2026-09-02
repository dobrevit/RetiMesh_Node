// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiIncoming.cpp — the full-screen interrupt a fresh message earns
//
//  Not a corner toast: a field device is glanced at, not watched, so an
//  arrival takes the glass — sender, the message itself, and the two honest
//  choices. The display task wakes the panel before calling here.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"

namespace {
lv_obj_t* sAlert = nullptr;
}

namespace Ui {

void showIncoming(const uint8_t from[16], const char* text) {
  char sender[34];
  peerLabel(from, sender, sizeof(sender));
  if (sAlert) { lv_obj_delete(sAlert); sAlert = nullptr; }  // newest wins
  sAlert = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sAlert);
  lv_obj_set_size(sAlert, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(sAlert, lv_color_hex(UiTheme::kGround), 0);
  lv_obj_set_style_bg_opa(sAlert, LV_OPA_COVER, 0);
  lv_obj_add_flag(sAlert, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(sAlert, [](lv_event_t*) { sAlert = nullptr; }, LV_EVENT_DELETE, nullptr);

  lv_obj_t* head = lv_label_create(sAlert);
  lv_label_set_text(head, "INCOMING LXMF");
  UiTheme::labelCaps(head);
  lv_obj_set_style_text_color(head, lv_color_hex(UiTheme::kWarn), 0);
  lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t* who = lv_label_create(sAlert);
  lv_label_set_text(who, sender);
  lv_obj_set_style_text_font(who, &font_barlow_16, 0);
  lv_obj_align(who, LV_ALIGN_TOP_MID, 0, 58);

  lv_obj_t* body = lv_label_create(sAlert);
  lv_label_set_text(body, text);
  lv_obj_set_style_text_color(body, lv_color_hex(UiTheme::kInkDim), 0);
  lv_obj_set_width(body, lv_pct(88));
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 92);

  struct Btn { const char* label; bool open; };
  static const Btn kBtns[] = { {"DISMISS", false}, {"OPEN", true} };
  lv_obj_t* row = lv_obj_create(sAlert);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(94), 44);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 6, 0);
  for (const Btn& b : kBtns) {
    lv_obj_t* btn = lv_button_create(row);
    UiTheme::actionButton(btn);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, b.label);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
      const bool open = (bool)(uintptr_t)lv_event_get_user_data(e);
      if (sAlert) lv_obj_delete(sAlert);         // DELETE hook clears the pointer
      if (open) Ui::openMessages();
    }, LV_EVENT_CLICKED, (void*)(uintptr_t)b.open);
  }
}

} // namespace Ui

#endif // HAS_LVGL_UI
