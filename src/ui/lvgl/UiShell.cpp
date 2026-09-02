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
//  UiShell.cpp — buffers, input, the status bar, the keyboard, navigation
//
//  The mobile chrome lives here. The status bar is the phone convention the
//  operator already knows: who I am on the left, how I am doing on the right
//  — radio, Wi-Fi, GNSS, battery, and the clock once the receiver has set it.
//  It sits on the top layer, so every screen inherits it without carrying it.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "TftPanel.h"
#include "TouchInput.h"
#include "Settings.h"
#include "Power.h"
#include "Gps.h"
#include "WifiManager.h"

namespace {

TftPanel* sPanel = nullptr;

constexpr int32_t kBarH = 22;

// Two partial render buffers, ~1/10 of the panel each, in internal DRAM:
// DMA-friendly, and the PSRAM-over-SPI chunking trap never applies.
constexpr size_t kBufPx = DISPLAY_WIDTH * 32;
static uint8_t sBuf1[kBufPx * 2];
static uint8_t sBuf2[kBufPx * 2];

lv_obj_t* sKeyboard = nullptr;
lv_obj_t* sBar = nullptr;
lv_obj_t* sBarName = nullptr;
lv_obj_t* sBarIcons = nullptr;

// The stack under the active screen. Six is deeper than any path here goes.
lv_obj_t* sStack[6];
uint8_t   sDepth = 0;

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const uint32_t n = (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px, n);              // ST7789 eats big-endian RGB565
  sPanel->blitArea((int16_t)area->x1, (int16_t)area->y1,
                   (int16_t)area->x2, (int16_t)area->y2, px);
  lv_display_flush_ready(disp);
}

void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  const TouchInput::Point p = TouchInput::poll();
  data->state = p.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (p.down) { data->point.x = p.x; data->point.y = p.y; }
}

// --- the status bar ---------------------------------------------------------

const char* batterySymbol(const Power::Battery& b) {
  if (!b.present)      return LV_SYMBOL_BATTERY_EMPTY;
  if (b.percent >= 85) return LV_SYMBOL_BATTERY_FULL;
  if (b.percent >= 60) return LV_SYMBOL_BATTERY_3;
  if (b.percent >= 35) return LV_SYMBOL_BATTERY_2;
  if (b.percent >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

void barTick(lv_timer_t*) {
  char text[96];
  size_t n = 0;
  auto add = [&](const char* s) { n += snprintf(text + n, sizeof(text) - n, "%s", s); };

  // The clock, once the receiver has set it: hours and minutes are the part a
  // person wants; the date belongs to the About dialog.
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet && strlen(f.utc) >= 16) {
    char hm[6] = { f.utc[11], f.utc[12], f.utc[13], f.utc[14], f.utc[15], 0 };
    add(hm); add("  ");
  }
  if (f.valid) { add(LV_SYMBOL_GPS); add(" "); }
#endif
  if (settings.links().wifiEnabled) { add(LV_SYMBOL_WIFI); add(" "); }
  add(g_stats.radioOnline ? "LoRa " : "");
  const Power::Battery b = Power::battery();
  if (b.chargeKnown && b.charging) add(LV_SYMBOL_CHARGE);
  add(batterySymbol(b));
  if (b.present) { char p[8]; snprintf(p, sizeof(p), " %u%%", b.percent); add(p); }

  lv_label_set_text(sBarIcons, text);
  lv_label_set_text(sBarName, wifiManager.ssid());
}

void barCreate() {
  sBar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sBar);
  lv_obj_set_size(sBar, LV_HOR_RES, kBarH);
  lv_obj_align(sBar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(sBar, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(sBar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(sBar, 6, 0);

  sBarName = lv_label_create(sBar);
  lv_obj_set_style_text_color(sBarName, lv_color_white(), 0);
  lv_obj_align(sBarName, LV_ALIGN_LEFT_MID, 0, 0);

  sBarIcons = lv_label_create(sBar);
  lv_obj_set_style_text_color(sBarIcons, lv_color_white(), 0);
  lv_obj_align(sBarIcons, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_timer_create(barTick, 1000, nullptr);
  barTick(nullptr);
}

// --- the keyboard -----------------------------------------------------------

void kbEvent(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

void taFocusEvent(lv_event_t* e) {
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    const bool numeric = (bool)(uintptr_t)lv_event_get_user_data(e);
    lv_keyboard_set_mode(sKeyboard, numeric ? LV_KEYBOARD_MODE_NUMBER
                                            : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(sKeyboard, ta);
    lv_obj_remove_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sKeyboard);
    // A modal dialog is lifted clear of the keys; a field on an ordinary
    // screen scrolls clear instead — its form has the bottom padding for it.
    lv_obj_t* dlg = ta;
    lv_obj_t* up  = lv_obj_get_parent(dlg);
    while (up && up != lv_layer_top() && up != lv_screen_active()) {
      dlg = up;
      up = lv_obj_get_parent(dlg);
    }
    if (up == lv_layer_top() && dlg != sKeyboard) {
      lv_obj_t* box = dlg;
      for (lv_obj_t* m = ta; m && m != dlg; m = lv_obj_get_parent(m))
        if (lv_obj_get_parent(m) == dlg) { box = m; break; }
      const int32_t kbTop = LV_VER_RES - lv_obj_get_height(sKeyboard);
      lv_obj_set_style_max_height(box, kbTop - kBarH - 8, 0);
      lv_obj_align(box, LV_ALIGN_TOP_MID, 0, kBarH + 2);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
  } else if (code == LV_EVENT_DEFOCUSED) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

void backEvent(lv_event_t*) { Ui::back(); }

} // namespace

namespace Ui {

bool shellInit(TftPanel& panel) {
  sPanel = &panel;
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  lv_display_t* disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!disp) return false;
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, sBuf1, sBuf2, sizeof(sBuf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touchCb);

  barCreate();

  sKeyboard = lv_keyboard_create(lv_layer_top());
  lv_obj_set_height(sKeyboard, 132);     // four ~33 px rows: tappable, compact
  lv_keyboard_set_mode(sKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(sKeyboard, kbEvent, LV_EVENT_ALL, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);

  log_i("gui: LVGL %d.%d.%d shell up — status bar, %u B x2 render buffers",
        lv_version_major(), lv_version_minor(), lv_version_patch(),
        (unsigned)sizeof(sBuf1));
  return true;
}

uint32_t shellLoop() { return lv_timer_handler(); }

void push(lv_obj_t* screen) {
  if (sDepth >= sizeof(sStack) / sizeof(sStack[0])) return;
  sStack[sDepth++] = lv_screen_active();
  lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 150, 0, false);
}

void back() {
  if (!sDepth) return;
  // auto_del deletes the screen being left once the slide is over.
  lv_screen_load_anim(sStack[--sDepth], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 150, 0, true);
}

bool atRoot() { return sDepth == 0; }

lv_obj_t* newScreen(const char* title) {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_set_style_pad_top(scr, kBarH, 0);   // the status bar's lane

  lv_obj_t* col = lv_obj_create(scr);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(col, 4, 0);
  lv_obj_set_style_pad_row(col, 4, 0);

  if (title) {
    lv_obj_t* head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_t* backBtn = lv_button_create(head);
    lv_obj_set_size(backBtn, 40, 28);
    lv_obj_align(backBtn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* bl = lv_label_create(backBtn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(backBtn, backEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* tl = lv_label_create(head);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);
  }

  lv_obj_t* body = lv_obj_create(col);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 4, 0);
  // Room to scroll any field clear of the keyboard.
  lv_obj_set_style_pad_bottom(body, 140, 0);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  return body;
}

void toast(const char* text) {
  lv_obj_t* t = lv_label_create(lv_layer_top());
  lv_label_set_text(t, text);
  lv_obj_set_style_bg_opa(t, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(t, lv_color_black(), 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_pad_all(t, 6, 0);
  lv_obj_set_width(t, LV_SIZE_CONTENT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, kBarH + 2);
  lv_obj_delete_delayed(t, 2500);
}

lv_obj_t* textarea(lv_obj_t* parent, const char* placeholder,
                   bool oneLine, bool numeric) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, oneLine);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_FOCUSED, (void*)(uintptr_t)numeric);
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_DEFOCUSED, nullptr);
  return ta;
}

} // namespace Ui

#endif // HAS_LVGL_UI
