// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiGps.cpp — position, in the type it deserves
//
//  Coordinates get the largest face on the screen — they are what gets read
//  aloud over voice — with MGRS beneath for users who work in grid. The
//  readings follow, and the sky view behind its button is a bar chart of
//  C/N0 per space vehicle: at this size elevation and azimuth are
//  unreadable, but signal strength per SV is the thing that explains a bad
//  fix.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI && HAS_GPS

#include <Arduino.h>
#include "UiTheme.h"
#include "Gps.h"
#include "Mgrs.h"
#include "Settings.h"
#include "SettingsFields.h"

namespace {

lv_obj_t* sLat = nullptr;
lv_obj_t* sLon = nullptr;
lv_obj_t* sMgrs = nullptr;
lv_obj_t* sFixVal = nullptr;
lv_obj_t* sSatVal = nullptr;
lv_obj_t* sAltVal = nullptr;
lv_obj_t* sSpdVal = nullptr;
lv_obj_t* sHdopVal = nullptr;
lv_obj_t* sClkVal = nullptr;
lv_obj_t* sShareLbl = nullptr;

void set(lv_obj_t* l, const char* t) {
  if (l && lv_obj_is_valid(l) && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

lv_obj_t* reading(lv_obj_t* parent, const char* label) {
  lv_obj_t* row = lv_obj_create(parent);
  UiTheme::card(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(row, 8, 0);
  lv_obj_set_style_pad_ver(row, 4, 0);
  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, label);
  UiTheme::labelCaps(l);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* v = lv_label_create(row);
  UiTheme::value(v);
  lv_label_set_text(v, "—");
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

void refresh(lv_timer_t*) {
  if (!sLat || !lv_obj_is_valid(sLat)) return;
  const Gps::Fix f = Gps::fix();
  char v[48];

  if (f.valid) {
    snprintf(v, sizeof(v), "%.5f°%c", fabs(f.latitude), f.latitude >= 0 ? 'N' : 'S');
    set(sLat, v);
    snprintf(v, sizeof(v), "%.5f°%c", fabs(f.longitude), f.longitude >= 0 ? 'E' : 'W');
    set(sLon, v);
    char g[32];
    if (Mgrs::fromLatLon(f.latitude, f.longitude, g, sizeof(g))) {
      snprintf(v, sizeof(v), "MGRS %s", g);
      set(sMgrs, v);
    } else set(sMgrs, "");
  } else {
    set(sLat, "--.-----");
    set(sLon, "--.-----");
    set(sMgrs, f.enabled ? (f.sentences ? "searching..." : "no data from receiver")
                         : "receiver off (settings: radio)");
  }

  set(sFixVal, f.valid ? "3D" : (f.sentences ? "searching" : "none"));
  lv_obj_set_style_text_color(sFixVal,
      lv_color_hex(f.valid ? UiTheme::kGood : UiTheme::kWarn), 0);
  snprintf(v, sizeof(v), "%u", f.satellites);           set(sSatVal, v);
  snprintf(v, sizeof(v), "%.0f m", (double)f.altitude); set(sAltVal, v);
  snprintf(v, sizeof(v), "%.1f km/h", (double)f.speedKmh); set(sSpdVal, v);
  snprintf(v, sizeof(v), "%.1f", (double)f.hdop);       set(sHdopVal, v);
  set(sClkVal, f.clockSet ? "GNSS" : "internal");
}

// --- the sky view -----------------------------------------------------------

lv_obj_t* sSkyCol = nullptr;
lv_obj_t* sSkyCaption = nullptr;

void skyRefresh(lv_timer_t*) {
  if (!sSkyCol || !lv_obj_is_valid(sSkyCol)) return;
  Gps::Sv sv[20];
  size_t n = Gps::skyView(sv, 20);
  // Strongest first — the bars should read like a story, not a lottery.
  for (size_t i = 0; i + 1 < n; i++)
    for (size_t j = 0; j + 1 < n - i; j++)
      if (sv[j].cn0 < sv[j + 1].cn0) { auto t = sv[j]; sv[j] = sv[j + 1]; sv[j + 1] = t; }

  lv_obj_clean(sSkyCol);
  if (!n) {
    lv_obj_t* none = lv_label_create(sSkyCol);
    lv_label_set_text(none, "nothing tracked");
    lv_obj_set_style_text_color(none, lv_color_hex(UiTheme::kInkLabel), 0);
  }
  const size_t shown = n < 10 ? n : 10;
  for (size_t i = 0; i < shown; i++) {
    lv_obj_t* row = lv_obj_create(sSkyCol);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 20);
    lv_obj_t* id = lv_label_create(row);
    char t[12];
    snprintf(t, sizeof(t), "%s %02u", sv[i].talker, sv[i].id);
    lv_label_set_text(id, t);
    lv_obj_set_style_text_color(id, lv_color_hex(UiTheme::kInkDim), 0);
    lv_obj_align(id, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* bar = lv_bar_create(row);
    lv_bar_set_range(bar, 0, 50);
    lv_bar_set_value(bar, sv[i].cn0, LV_ANIM_OFF);
    lv_obj_set_size(bar, lv_pct(58), 8);
    lv_obj_align(bar, LV_ALIGN_CENTER, 14, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(
        sv[i].cn0 >= 35 ? UiTheme::kGood : (sv[i].cn0 >= 20 ? UiTheme::kWarn : UiTheme::kEdge)),
        LV_PART_INDICATOR);
    lv_obj_t* num = lv_label_create(row);
    snprintf(t, sizeof(t), "%u", sv[i].cn0);
    lv_label_set_text(num, t);
    lv_obj_align(num, LV_ALIGN_RIGHT_MID, 0, 0);
  }
  const Gps::Fix f = Gps::fix();
  char cap[40];
  snprintf(cap, sizeof(cap), "%u used · %u tracked", f.satellites, (unsigned)n);
  set(sSkyCaption, cap);
}

void openSky(lv_event_t*) {
  lv_obj_t* body = Ui::newScreen("Sky view");
  lv_obj_t* head = lv_label_create(body);
  lv_label_set_text(head, "C/N0  dB-Hz");
  UiTheme::labelCaps(head);
  sSkyCol = lv_obj_create(body);
  lv_obj_remove_style_all(sSkyCol);
  lv_obj_set_width(sSkyCol, lv_pct(100));
  lv_obj_set_flex_grow(sSkyCol, 1);
  lv_obj_set_flex_flow(sSkyCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sSkyCol, 4, 0);
  sSkyCaption = lv_label_create(body);
  lv_obj_set_style_text_color(sSkyCaption, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_label_set_text(sSkyCaption, "");
  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  lv_timer_t* t = lv_timer_create(skyRefresh, 2000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sSkyCol = nullptr;
  }, LV_EVENT_DELETE, t);
  skyRefresh(nullptr);
  Ui::push(scr);
}

void shareLabel() {
  char t[24];
  snprintf(t, sizeof(t), "SHARE POS: %s", settings.radio().gpsSharePosition ? "on" : "off");
  set(sShareLbl, t);
}

} // namespace

namespace Ui {

void openGps() {
  lv_obj_t* body = newScreen("Position");

  sLat = lv_label_create(body);
  lv_obj_set_style_text_font(sLat, &font_barlow_28, 0);
  lv_label_set_text(sLat, "--.-----");
  sLon = lv_label_create(body);
  lv_obj_set_style_text_font(sLon, &font_barlow_28, 0);
  lv_label_set_text(sLon, "--.-----");
  sMgrs = lv_label_create(body);
  lv_obj_set_style_text_color(sMgrs, lv_color_hex(UiTheme::kInkDim), 0);
  lv_label_set_text(sMgrs, "");

  sFixVal  = reading(body, "FIX");
  sSatVal  = reading(body, "SATELLITES");
  sAltVal  = reading(body, "ALTITUDE");
  sSpdVal  = reading(body, "SPEED");
  sHdopVal = reading(body, "HDOP");
  sClkVal  = reading(body, "CLOCK SOURCE");

  lv_obj_t* row = lv_obj_create(body);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 40);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_t* sky = lv_button_create(row);
  UiTheme::actionButton(sky);
  lv_obj_set_flex_grow(sky, 1);
  lv_obj_set_height(sky, lv_pct(100));
  lv_obj_t* kl = lv_label_create(sky);
  lv_label_set_text(kl, "SKY VIEW");
  lv_obj_center(kl);
  lv_obj_add_event_cb(sky, openSky, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* share = lv_button_create(row);
  UiTheme::actionButton(share);
  lv_obj_set_flex_grow(share, 1);
  lv_obj_set_height(share, lv_pct(100));
  sShareLbl = lv_label_create(share);
  lv_obj_center(sShareLbl);
  shareLabel();
  lv_obj_add_event_cb(share, [](lv_event_t*) {
    // Through the funnel, like every settings change from any face.
    char err[96] = "";
    const bool want = !settings.radio().gpsSharePosition;
    const SettingsFields::Result r =
        SettingsFields::set("radio.gps_share_position", want ? "on" : "off", err, sizeof(err));
    if (r == SettingsFields::Result::Ok) shareLabel();
    else Ui::toast(err[0] ? err : SettingsFields::resultText(r));
  }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  lv_timer_t* t = lv_timer_create(refresh, 1000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sLat = nullptr;
  }, LV_EVENT_DELETE, t);
  refresh(nullptr);
  push(scr);
}

} // namespace Ui

#endif // HAS_LVGL_UI && HAS_GPS
