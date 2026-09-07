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


void tick(lv_timer_t*) {
  if (!sPanel) return;
  char v[64];
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet && strlen(f.utc) >= 16) {
    char hm[6] = { f.utc[11], f.utc[12], f.utc[13], f.utc[14], f.utc[15], 0 };
    Ui::setLabel(sTime, hm);
    snprintf(v, sizeof(v), "%.10s UTC", f.utc);
    Ui::setLabel(sDate, v);
  } else { Ui::setLabel(sTime, "--:--"); Ui::setLabel(sDate, ""); }
  // This is the screen the device spends its life on, and with the claim above
  // withheld it is also the screen a receiver rests under — so it is the one
  // place a satellite count frozen for five minutes would be read as a fault.
  // The same word the GPS page, the mono page and /api/status use, rather than
  // a fourth way of saying it.
  if (f.portFault) snprintf(v, sizeof(v), "GNSS no port");
  else if (f.resting) snprintf(v, sizeof(v), "GNSS resting");
  else if (f.valid) snprintf(v, sizeof(v), "GNSS 3D · %u sv", f.satellites);
  else if (f.enabled) snprintf(v, sizeof(v), "GNSS searching");
  else snprintf(v, sizeof(v), "GNSS off");
  Ui::setLabel(sGnss, v);
#else
  Ui::setLabel(sTime, "--:--");
#endif
  snprintf(v, sizeof(v), "LISTENING  %.3f MHz", (double)settings.radio().freqMhz);
  Ui::setLabel(sListen, v);
  const Power::Battery b = Power::battery();
  if (b.present) snprintf(v, sizeof(v), "TAP TO WAKE  ·  %u%%", b.percent);
  else snprintf(v, sizeof(v), "TAP TO WAKE");
  Ui::setLabel(sFoot, v);
}

void build() {
  sPanel = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sPanel);
  lv_obj_set_size(sPanel, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(sPanel, lv_color_hex(UiTheme::kGround), 0);
  lv_obj_set_style_bg_opa(sPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(sPanel, LV_OBJ_FLAG_CLICKABLE);   // the waking tap ends here

  sTime = lv_label_create(sPanel);
  lv_obj_set_style_text_font(sTime, &font_barlow_28, 0);
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

void resetIdle() {
  // The panel is built once and kept; a theme change deletes it so the
  // next rest is born in the current palette.
  if (sTick) { lv_timer_delete(sTick); sTick = nullptr; }
  if (sPanel) { lv_obj_delete(sPanel); sPanel = nullptr; }
}

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

// The claim, refused while this clock is over the page making it. showIdle()
// hides an overlay on the top layer; the screen beneath keeps its widgets and
// its timers, which only die on LV_EVENT_DELETE — so the GPS page's refresh,
// the sky view's and the bearing dial's all go on running under the clock. The
// glass is still lit at this stage (the shell blanks only after four quiet
// timeouts), so Power::screenDark() cannot answer this: it means the panel is
// off, and the compass and accelerometer follow it. What is true here is
// narrower and belongs to the UI — a page that is painting behind another one
// is not a page anybody is reading — and left unsaid it deferred the first
// rest by the whole idle stage, up to four minutes on the default profile.
void navPainted() {
#if HAS_GPS
  if (!idleShowing()) Gps::navViewPainted();
#endif
}

} // namespace Ui

#endif // HAS_LVGL_UI
