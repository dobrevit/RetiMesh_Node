// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiNav.cpp — the bearing dial and the relative plot, and the firmware
//  overlay that owns the glass while an install runs
//
//  No basemap, no tiles — a dial and rings are what this MCU can promise.
//  Peers do not yet announce positions on this mesh, and both instruments
//  say so instead of pointing somewhere invented; the day positions ride
//  the announces, these screens have their geometry waiting.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"
#include "Gps.h"
#include "PeerPositions.h"
#include "GeoMath.h"
#include "RnsTransport.h"
#include <math.h>

namespace {

lv_obj_t* ring(lv_obj_t* parent, int d, uint32_t colorHex) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, d, d);
  lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_set_style_border_color(r, lv_color_hex(colorHex), 0);
  lv_obj_align(r, LV_ALIGN_CENTER, 0, 0);
  return r;
}

void compassLetters(lv_obj_t* parent, int radius) {
  struct { const char* t; int x, y; } pts[] = {
    { "N", 0, -radius }, { "E", radius, 0 }, { "S", 0, radius }, { "W", -radius, 0 } };
  for (auto& p : pts) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, p.t);
    lv_obj_set_style_text_color(l, lv_color_hex(UiTheme::kInkLabel), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, p.x, p.y);
  }
}

// --- the firmware overlay ---------------------------------------------------

lv_obj_t* sFw = nullptr;
lv_obj_t* sFwStage = nullptr;
lv_obj_t* sFwBar = nullptr;
lv_obj_t* sFwPct = nullptr;

} // namespace

namespace Ui {

namespace {
// The dial's live state, one open screen at a time like every screen here.
char       sBearingHash[33];
lv_obj_t*  sDialDeg = nullptr;
lv_obj_t*  sDialDist = nullptr;
lv_obj_t*  sDialWhy = nullptr;
lv_obj_t*  sNeedle = nullptr;
lv_point_precise_t sNeedlePts[2];

void bearingTick(lv_timer_t*) {
  if (!sDialDeg || !lv_obj_is_valid(sDialDeg)) return;
  char v[64];
#if HAS_GPS
  const Gps::Fix own = Gps::fix();
  PeerPositions::Position pp;
  const bool have = own.valid && PeerPositions::getByHex(sBearingHash, pp);
  if (have) {
    const double b = GeoMath::bearingDeg(own.latitude, own.longitude,
                                         pp.latitude, pp.longitude);
    const double km = GeoMath::distanceKm(own.latitude, own.longitude,
                                          pp.latitude, pp.longitude);
    snprintf(v, sizeof(v), "%.0f°", b);
    Ui::setLabel(sDialDeg, v);
    snprintf(v, sizeof(v), km < 10.0 ? "%.2f km" : "%.1f km", km);
    Ui::setLabel(sDialDist, v);
    char age[8];
    Ui::ageTextS((millis() - pp.heardMs) / 1000, age, sizeof(age));
    snprintf(v, sizeof(v), "pos heard %s ago · ±%.0f m", age, (double)pp.accuracyM);
    Ui::setLabel(sDialWhy, v);
    // The needle: centre to the ring's edge at the bearing, north up.
    const double rad = b * M_PI / 180.0;
    sNeedlePts[0].x = 0;  sNeedlePts[0].y = 0;
    sNeedlePts[1].x = (lv_value_precise_t)(sin(rad) * 66.0);
    sNeedlePts[1].y = (lv_value_precise_t)(-cos(rad) * 66.0);
    lv_line_set_points(sNeedle, sNeedlePts, 2);
    lv_obj_remove_flag(sNeedle, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  Ui::setLabel(sDialDeg, "—°");
  Ui::setLabel(sDialDist, "");
  Ui::setLabel(sDialWhy, own.valid
      ? "This peer has not announced a position yet."
      : "Own position: no fix — the dial needs both ends.");
  lv_obj_add_flag(sNeedle, LV_OBJ_FLAG_HIDDEN);
#endif
}
} // namespace

void openBearing(const char* peer, const char* hashHex) {
  snprintf(sBearingHash, sizeof(sBearingHash), "%s", hashHex ? hashHex : "");
  lv_obj_t* body = newScreen(peer);
  lv_obj_t* dial = lv_obj_create(body);
  lv_obj_remove_style_all(dial);
  lv_obj_set_size(dial, lv_pct(100), 180);
  ring(dial, 150, UiTheme::kEdge);
  compassLetters(dial, 84);
  sNeedle = lv_line_create(dial);
  lv_obj_set_style_line_width(sNeedle, 3, 0);
  lv_obj_set_style_line_color(sNeedle, lv_color_hex(UiTheme::kGood), 0);
  lv_obj_set_style_line_rounded(sNeedle, true, 0);
  lv_obj_align(sNeedle, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(sNeedle, LV_OBJ_FLAG_HIDDEN);
  ring(dial, 6, UiTheme::kAccent);
  sDialDeg = lv_label_create(dial);
  lv_label_set_text(sDialDeg, "—°");
  lv_obj_set_style_text_font(sDialDeg, &font_barlow_28, 0);
  lv_obj_align(sDialDeg, LV_ALIGN_CENTER, 0, -26);
  sDialDist = lv_label_create(dial);
  lv_label_set_text(sDialDist, "");
  lv_obj_align(sDialDist, LV_ALIGN_CENTER, 0, 30);

  sDialWhy = lv_label_create(body);
  lv_label_set_text(sDialWhy, "");
  lv_obj_set_style_text_color(sDialWhy, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_obj_set_width(sDialWhy, lv_pct(100));
  lv_label_set_long_mode(sDialWhy, LV_LABEL_LONG_WRAP);

  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  lv_timer_t* t = lv_timer_create(bearingTick, 1000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sDialDeg = nullptr;
  }, LV_EVENT_DELETE, t);
  bearingTick(nullptr);
  push(scr);
}

void openPlot() {
  lv_obj_t* body = newScreen("Plot");
  lv_obj_t* field = lv_obj_create(body);
  lv_obj_remove_style_all(field);
  lv_obj_set_width(field, lv_pct(100));
  lv_obj_set_flex_grow(field, 1);
  ring(field, 100, UiTheme::kEdge);
  ring(field, 200, UiTheme::kEdge);
  lv_obj_t* self = ring(field, 8, UiTheme::kGood);
  lv_obj_set_style_bg_color(self, lv_color_hex(UiTheme::kGood), 0);
  lv_obj_set_style_bg_opa(self, LV_OPA_COVER, 0);
  compassLetters(field, 104);
  lv_obj_t* r1 = lv_label_create(field);
  lv_label_set_text(r1, "1 km");
  lv_obj_set_style_text_color(r1, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_obj_align(r1, LV_ALIGN_CENTER, 38, -38);

  // Peers with announced positions, placed north-up from our own fix:
  // ring one is a kilometre, the rim two, and beyond clamps to the rim —
  // a plot, not a map, exactly as the spec drew it.
  size_t placed = 0;
#if HAS_GPS
  const Gps::Fix own = Gps::fix();
  if (own.valid) {
    RnsTransport::PathInfo paths[16];
    const size_t n = RnsTransport::paths(paths, 16);
    for (size_t i = 0; i < n && placed < 8; i++) {
      PeerPositions::Position pp;
      if (!PeerPositions::getByHex(paths[i].hash, pp)) continue;
      const double km = GeoMath::distanceKm(own.latitude, own.longitude,
                                            pp.latitude, pp.longitude);
      const double b = GeoMath::bearingDeg(own.latitude, own.longitude,
                                           pp.latitude, pp.longitude) * M_PI / 180.0;
      double r = km * 50.0;              // 50 px per km, ring one at 1 km
      if (r > 104.0) r = 104.0;          // beyond the rim sits on the rim
      const int x = (int)(sin(b) * r), y = (int)(-cos(b) * r);
      lv_obj_t* dot = ring(field, 8, UiTheme::kAccent);
      lv_obj_set_style_bg_color(dot, lv_color_hex(UiTheme::kAccent), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      lv_obj_align(dot, LV_ALIGN_CENTER, x, y);
      char who[34];
      Ui::peerLabelHex(paths[i].hash, who, sizeof(who));
      lv_obj_t* tag = lv_label_create(field);
      lv_label_set_text_fmt(tag, "%.10s", who);
      lv_obj_set_style_text_color(tag, lv_color_hex(UiTheme::kInkDim), 0);
      lv_obj_align(tag, LV_ALIGN_CENTER, x, y + 12);
      placed++;
    }
  }
#endif
  lv_obj_t* cap = lv_label_create(body);
  if (placed)
    lv_label_set_text_fmt(cap, "NORTH UP · %u peer%s placed", (unsigned)placed,
                          placed == 1 ? "" : "s");
  else
    lv_label_set_text(cap, "NORTH UP · no peer positions on the air yet");
  lv_obj_set_style_text_color(cap, lv_color_hex(UiTheme::kInkLabel), 0);
  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

void showFirmware(const char* stage, uint32_t written, uint32_t total) {
  if (!sFw) {
    sFw = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sFw);
    lv_obj_set_size(sFw, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sFw, lv_color_hex(UiTheme::kGround), 0);
    lv_obj_set_style_bg_opa(sFw, LV_OPA_COVER, 0);
    lv_obj_add_flag(sFw, LV_OBJ_FLAG_CLICKABLE);   // nothing beneath is tappable

    lv_obj_t* head = lv_label_create(sFw);
    lv_label_set_text(head, "FIRMWARE");
    UiTheme::labelCaps(head);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 40);

    sFwStage = lv_label_create(sFw);
    lv_obj_set_style_text_font(sFwStage, &font_barlow_16, 0);
    lv_obj_align(sFwStage, LV_ALIGN_TOP_MID, 0, 66);

    sFwBar = lv_bar_create(sFw);
    lv_obj_set_size(sFwBar, lv_pct(80), 10);
    lv_obj_align(sFwBar, LV_ALIGN_CENTER, 0, -10);
    lv_bar_set_range(sFwBar, 0, 100);

    sFwPct = lv_label_create(sFw);
    lv_obj_set_style_text_color(sFwPct, lv_color_hex(UiTheme::kInkDim), 0);
    lv_obj_align(sFwPct, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t* warn = lv_label_create(sFw);
    lv_label_set_text(warn, "DO NOT POWER OFF");
    lv_obj_set_style_text_color(warn, lv_color_hex(UiTheme::kWarn), 0);
    lv_obj_align(warn, LV_ALIGN_CENTER, 0, 44);

    // The absent option is explained rather than merely missing.
    lv_obj_t* cancel = lv_button_create(sFw);
    UiTheme::actionButton(cancel);
    lv_obj_add_state(cancel, LV_STATE_DISABLED);
    lv_obj_set_size(cancel, lv_pct(80), 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "CANCEL UNAVAILABLE");
    lv_obj_center(cl);
  }
  lv_obj_remove_flag(sFw, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(sFw);
  char t[24];
  snprintf(t, sizeof(t), "%s", stage);
  if (strcmp(lv_label_get_text(sFwStage), t)) lv_label_set_text(sFwStage, t);
  const uint32_t pct = total ? (uint32_t)((uint64_t)written * 100 / total) : 0;
  lv_bar_set_value(sFwBar, (int32_t)pct, LV_ANIM_OFF);
  snprintf(t, sizeof(t), "%lu / %lu KB", (unsigned long)(written / 1024),
           (unsigned long)(total / 1024));
  if (strcmp(lv_label_get_text(sFwPct), t)) lv_label_set_text(sFwPct, t);
}

void hideFirmware() {
  if (sFw) lv_obj_add_flag(sFw, LV_OBJ_FLAG_HIDDEN);
}

} // namespace Ui

#endif // HAS_LVGL_UI
