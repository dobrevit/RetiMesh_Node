// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiIdle.cpp — the screen the device spends its life on
//
//  After the quiet timeout the UI collapses to this: the clock the operator
//  actually reads, the receiver's state, the frequency the radio is holding,
//  and how much battery is left — on the darkest ground the theme has. A tap
//  or a key wakes the full shell; the overlay absorbs that tap so waking is
//  all it does.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"
#include "Settings.h"
#include "Power.h"
#include "Gps.h"

namespace {

lv_obj_t*   sPanel = nullptr;
lv_obj_t*   sTime = nullptr;
lv_obj_t*   sDate = nullptr;
lv_obj_t*   sGnss = nullptr;
lv_obj_t*   sListen = nullptr;
lv_obj_t*   sFoot = nullptr;
lv_timer_t* sTick = nullptr;

void set(lv_obj_t* l, const char* t) {
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

void tick(lv_timer_t*) {
  if (!sPanel) return;
  char v[64];
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet && strlen(f.utc) >= 16) {
    char hm[6] = { f.utc[11], f.utc[12], f.utc[13], f.utc[14], f.utc[15], 0 };
    set(sTime, hm);
    snprintf(v, sizeof(v), "%.10s UTC", f.utc);
    set(sDate, v);
  } else { set(sTime, "--:--"); set(sDate, ""); }
  if (f.valid) snprintf(v, sizeof(v), "GNSS 3D · %u sv", f.satellites);
  else if (f.enabled) snprintf(v, sizeof(v), "GNSS searching");
  else snprintf(v, sizeof(v), "GNSS off");
  set(sGnss, v);
#else
  set(sTime, "--:--");
#endif
  snprintf(v, sizeof(v), "LISTENING  %.3f MHz", (double)settings.radio().freqMhz);
  set(sListen, v);
  const Power::Battery b = Power::battery();
  if (b.present) snprintf(v, sizeof(v), "TAP TO WAKE  ·  %u%%", b.percent);
  else snprintf(v, sizeof(v), "TAP TO WAKE");
  set(sFoot, v);
}

void build() {
  sPanel = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sPanel);
  lv_obj_set_size(sPanel, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(sPanel, lv_color_hex(UiTheme::kGround), 0);
  lv_obj_set_style_bg_opa(sPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(sPanel, LV_OBJ_FLAG_CLICKABLE);   // the waking tap ends here

  sTime = lv_label_create(sPanel);
  lv_obj_set_style_text_font(sTime, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sTime, lv_color_hex(UiTheme::kInk), 0);
  lv_label_set_text(sTime, "--:--");
  lv_obj_align(sTime, LV_ALIGN_CENTER, 0, -46);

  sDate = lv_label_create(sPanel);
  lv_obj_set_style_text_color(sDate, lv_color_hex(UiTheme::kInkDim), 0);
  lv_label_set_text(sDate, "");
  lv_obj_align(sDate, LV_ALIGN_CENTER, 0, -14);

  sGnss = lv_label_create(sPanel);
  lv_obj_set_style_text_color(sGnss, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_label_set_text(sGnss, "");
  lv_obj_align(sGnss, LV_ALIGN_CENTER, 0, 14);

  sListen = lv_label_create(sPanel);
  lv_obj_set_style_text_color(sListen, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_label_set_text(sListen, "");
  lv_obj_align(sListen, LV_ALIGN_CENTER, 0, 38);

  sFoot = lv_label_create(sPanel);
  lv_obj_set_style_text_color(sFoot, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_label_set_text(sFoot, "TAP TO WAKE");
  lv_obj_align(sFoot, LV_ALIGN_BOTTOM_MID, 0, -14);
}

} // namespace

namespace Ui {

void showIdle(bool on) {
  const bool showing = sPanel && !lv_obj_has_flag(sPanel, LV_OBJ_FLAG_HIDDEN);
  if (on == showing) return;
  if (on) {
    if (!sPanel) build();
    lv_obj_remove_flag(sPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sPanel);
    if (!sTick) sTick = lv_timer_create(tick, 1000, nullptr);
    tick(nullptr);
  } else {
    if (sPanel) lv_obj_add_flag(sPanel, LV_OBJ_FLAG_HIDDEN);
    if (sTick) { lv_timer_delete(sTick); sTick = nullptr; }
  }
}

bool idleShowing() {
  return sPanel && !lv_obj_has_flag(sPanel, LV_OBJ_FLAG_HIDDEN);
}

} // namespace Ui

#endif // HAS_LVGL_UI
