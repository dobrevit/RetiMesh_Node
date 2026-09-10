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
// The dial's live state — one owner at a time: the DELETE teardown checks
// the owning screen, so a stale deferred delete (back()'s 150 ms slide) can
// never null a freshly opened dial's statics.
char       sBearingHash[33];
lv_obj_t*  sDialOwner = nullptr;
lv_obj_t*  sDialDeg = nullptr;
lv_obj_t*  sDialDist = nullptr;
lv_obj_t*  sDialWhy = nullptr;
lv_obj_t*  sNeedle = nullptr;
lv_point_precise_t sNeedlePts[2];

// North-up polar to screen, stated once for the needle and the plot.
void northUpXY(double bearingDeg, double radius, int& x, int& y) {
  const double rad = bearingDeg * M_PI / 180.0;
  x = (int)(sin(rad) * radius);
  y = (int)(-cos(rad) * radius);
}

#if HAS_GPS
void bearingTick(lv_timer_t*) {
  if (!sDialDeg || !lv_obj_is_valid(sDialDeg)) return;
  // A live dial needs a live end of the baseline: while it is on the glass the
  // receiver tracks rather than duty-cycling under it — on the glass being the
  // part the shell's claim checks, since this timer outlives the idle clock
  // arriving over the dial.
  Ui::navPainted();
  char v[80];
  const Gps::Fix own = Gps::fix();
  PeerPositions::Position pp;
  const bool havePeer = PeerPositions::getByHex(sBearingHash, pp);
  if (havePeer && own.valid) {
    double km, deg;
    GeoMath::distanceAndBearing(own.latitude, own.longitude,
                                pp.latitude, pp.longitude, km, deg);
    snprintf(v, sizeof(v), "%.0f°", deg);
    Ui::setLabel(sDialDeg, v);
    char dist[16];
    Ui::formatKm(dist, sizeof(dist), km);
    Ui::setLabel(sDialDist, dist);
    char age[8];
    Ui::ageTextMs(millis() - pp.heardMs, age, sizeof(age));
    // Both ends of the baseline, honestly: their accuracy and ours.
    snprintf(v, sizeof(v), "pos heard %s ago · ±%.0f m · own ±%.0f m",
             age, (double)pp.accuracyM, (double)(own.hdop * 5.0f));
    Ui::setLabel(sDialWhy, v);
    // The needle lives in a positive-coordinate box pivoted at its centre —
    // raw negative points clip in LVGL and centre-alignment centres the
    // bounding box, which drew north invisible and east as a chord.
    int x, y;
    northUpXY(deg, 62.0, x, y);
    sNeedlePts[0].x = 70; sNeedlePts[0].y = 70;
    sNeedlePts[1].x = (lv_value_precise_t)(70 + x);
    sNeedlePts[1].y = (lv_value_precise_t)(70 + y);
    lv_line_set_points(sNeedle, sNeedlePts, 2);
    lv_obj_remove_flag(sNeedle, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  Ui::setLabel(sDialDeg, "—°");
  Ui::setLabel(sDialDist, "");
  if (havePeer) {
    char age[8];
    Ui::ageTextMs(millis() - pp.heardMs, age, sizeof(age));
    snprintf(v, sizeof(v), "Peer position held (%s ago) — own fix needed for a bearing.", age);
    Ui::setLabel(sDialWhy, v);
  } else {
    Ui::setLabel(sDialWhy, own.valid
        ? "This peer has not announced a position yet."
        : "Own position: no fix — the dial needs both ends.");
  }
  lv_obj_add_flag(sNeedle, LV_OBJ_FLAG_HIDDEN);
}

// --- the plot ---------------------------------------------------------------
//
// The rings, the compass letters and the dot in the middle are the screen and
// never move; what moves is where the peers sit relative to us, which is our
// own fix and theirs. So the peers get a transparent layer of their own over
// the field and that layer is the only thing a refresh rebuilds.
lv_obj_t* sPlotOwner = nullptr;
lv_obj_t* sPlotLayer = nullptr;
lv_obj_t* sPlotCap   = nullptr;
uint32_t  sPlotStamp = 0;
bool      sPlotDrawn = false;

// One peer's place on the glass, worked out before any widget is touched: the
// geometry is cheap and the widgets are not, so the rebuild below only happens
// when this has actually moved. Static rather than on the stack for the reason
// the path array is — this runs on the display task, whose deepest moment is
// widget-building, and LVGL gives it no second caller to race with.
struct Placed { int x, y; char who[12]; };
Placed sPlaced[8];

void plotRefresh(lv_timer_t*) {
  if (!sPlotLayer || !lv_obj_is_valid(sPlotLayer)) return;
  // A screen laid out around where the node is needs our end of the baseline,
  // so the receiver keeps looking under it — renewed on every tick rather than
  // claimed once at open, because a claim expires (GnssDutyPolicy) and this
  // screen is one somebody leaves up while walking. Through the shell's rule,
  // which refuses it while the idle clock is over the page.
  Ui::navPainted();

  // Off the display task's stack on purpose: this screen's deepest moment is
  // widget-building, and the sibling file keeps its path array static for the
  // same reason.
  static RnsTransport::PathInfo sPaths[24];
  size_t count = 0;
  const Gps::Fix own = Gps::fix();
  // One reading, under one lock — the list screen and Display.cpp's transport
  // panel take theirs the same way. Asked outside the fix check because the
  // caption below needs it either way: an empty plot on a node that has not
  // finished reading its own path table looks exactly like an empty plot on a
  // node nobody has announced to, and the two want different answers
  // (RnsTransport.h).
  const RnsTransport::Snapshot snap = RnsTransport::snapshot(sPaths, 24, nullptr, 0);
  if (own.valid) {
    // Peers with announced positions, north-up from our fix: ring one is a
    // kilometre, the rim two, and beyond clamps just inside the rim — not on
    // the compass letters.
    const size_t n = snap.pathRows;
    for (size_t i = 0; i < n && count < 8; i++) {
      PeerPositions::Position pp;
      if (!PeerPositions::getByHex(sPaths[i].hash, pp)) continue;
      double km, deg;
      GeoMath::distanceAndBearing(own.latitude, own.longitude,
                                  pp.latitude, pp.longitude, km, deg);
      double r = km * 50.0;
      if (r > 96.0) r = 96.0;            // inside the 100 px rim
      northUpXY(deg, r, sPlaced[count].x, sPlaced[count].y);
      char who[34];
      Ui::peerLabelHex(sPaths[i].hash, who, sizeof(who));
      snprintf(sPlaced[count].who, sizeof(sPlaced[count].who), "%.10s", who);
      count++;
    }
  }

  // An unchanged picture costs nothing, the sky view's rule (UiGps.cpp):
  // sixteen widgets torn down and rebuilt every couple of seconds for
  // identical readings is churn on the task the glass is painted from. The
  // caption's inputs are in the stamp too, since it is redrawn with them.
  const size_t held = PeerPositions::count();
  // A third caption, and so a third stamp input: the path rows are published
  // only once a pass has been all the way round the table, so a node bigger
  // than one pass has none to plot from and is not the same as a node with no
  // peer positions on the air.
  const bool stillReading = !snap.tables.snapRowsWhole && snap.pathTotal > 0;
  uint32_t stamp = (uint32_t)count * 131u + (uint32_t)held * 7u + (own.valid ? 1u : 0u);
  stamp = stamp * 31u + (stillReading ? (uint32_t)snap.pathTotal + 1u : 0u);
  for (size_t i = 0; i < count; i++) {
    stamp = stamp * 31u + (uint32_t)(sPlaced[i].x + 1024) * 2048u
                        + (uint32_t)(sPlaced[i].y + 1024);
    for (const char* c = sPlaced[i].who; *c; c++) stamp = stamp * 31u + (uint8_t)*c;
  }
  if (sPlotDrawn && stamp == sPlotStamp) return;
  sPlotStamp = stamp;
  sPlotDrawn = true;

  lv_obj_clean(sPlotLayer);
  for (size_t i = 0; i < count; i++) {
    lv_obj_t* dot = ring(sPlotLayer, 8, UiTheme::kAccent);
    lv_obj_set_style_bg_color(dot, lv_color_hex(UiTheme::kAccent), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, sPlaced[i].x, sPlaced[i].y);
    lv_obj_t* tag = lv_label_create(sPlotLayer);
    lv_label_set_text(tag, sPlaced[i].who);
    lv_obj_set_style_text_color(tag, lv_color_hex(UiTheme::kInkDim), 0);
    lv_obj_align(tag, LV_ALIGN_CENTER, sPlaced[i].x, sPlaced[i].y + 12);
  }
  if (count)
    lv_label_set_text_fmt(sPlotCap, "NORTH UP · %u peer%s placed", (unsigned)count,
                          count == 1 ? "" : "s");
  else if (held && (!own.valid || !stillReading))
    // The store knows the truth the old caption guessed at: positions are
    // on the air; the missing half is our own fix. Without a fix that is the
    // whole answer whatever the path table is doing, which is why it is asked
    // first; with one, a table still being read is the better explanation for
    // an empty plot and gets the caption instead.
    lv_label_set_text_fmt(sPlotCap, "NORTH UP · own fix needed — %u position%s held",
                          (unsigned)held, held == 1 ? "" : "s");
  else if (stillReading)
    lv_label_set_text_fmt(sPlotCap, "NORTH UP · still reading %u path%s",
                          (unsigned)snap.pathTotal, snap.pathTotal == 1 ? "" : "s");
  else
    lv_label_set_text(sPlotCap, "NORTH UP · no peer positions on the air yet");
}
#endif
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
  lv_obj_set_size(sNeedle, 140, 140);
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
  lv_obj_set_style_text_color(sDialWhy, lv_color_hex(UiTheme::kInkLabel), 0);
  lv_obj_set_width(sDialWhy, lv_pct(100));
  lv_label_set_long_mode(sDialWhy, LV_LABEL_LONG_WRAP);

  lv_obj_t* scr = Ui::screenOf(body);
  sDialOwner = scr;
#if HAS_GPS
  lv_label_set_text(sDialWhy, "");
  lv_timer_t* t = lv_timer_create(bearingTick, 1000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    // Only the owner tears the statics down: a stale deferred delete from
    // back()'s slide, or an older stacked dial dying, must not blind a
    // newer one.
    if ((lv_obj_t*)lv_event_get_target(e) == sDialOwner) {
      sDialOwner = nullptr;
      sDialDeg = nullptr; sDialDist = nullptr; sDialWhy = nullptr; sNeedle = nullptr;
    }
  }, LV_EVENT_DELETE, t);
  bearingTick(nullptr);
#else
  // No receiver in this build: say so once, and burn no timer on a dial
  // that can never point.
  lv_label_set_text(sDialWhy, "This build has no position receiver — "
                              "the dial needs our end of the baseline.");
#endif
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

  lv_obj_t* cap = lv_label_create(body);
  lv_obj_set_style_text_color(cap, lv_color_hex(UiTheme::kInkLabel), 0);
#if HAS_GPS
  // The peers get a layer of their own over the rings, because they are the
  // only part of this picture that moves and a refresh must not take the rings
  // with it. Transparent and unscrollable: it is a coordinate frame, not a
  // widget.
  sPlotLayer = lv_obj_create(field);
  lv_obj_remove_style_all(sPlotLayer);
  lv_obj_set_size(sPlotLayer, lv_pct(100), lv_pct(100));
  lv_obj_remove_flag(sPlotLayer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(sPlotLayer, LV_ALIGN_CENTER, 0, 0);
  sPlotCap = cap;
  sPlotDrawn = false;                    // this screen has drawn nothing yet

  // On a timer, not once at open. The claim navPainted() makes is served by
  // the GNSS task on its own pass, so the fix read a line later is still the
  // one the resting receiver last stood behind — and with nothing to redraw
  // the screen, the offsets plotted from it stayed that stale for as long as
  // the page was up. A claim that expires (GnssDutyPolicy) wants renewing
  // anyway. Two seconds, the sky view's beat rather than the dial's second:
  // this one rebuilds widgets, and the stamp in plotRefresh() means an
  // unchanged picture rebuilds nothing at all.
  lv_obj_t* scr = Ui::screenOf(body);
  sPlotOwner = scr;
  lv_timer_t* t = lv_timer_create(plotRefresh, 2000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    // Only the owner tears the statics down, the dial's rule above: a stale
    // deferred delete from back()'s slide must not blind a freshly opened one.
    if ((lv_obj_t*)lv_event_get_target(e) == sPlotOwner) {
      sPlotOwner = nullptr;
      sPlotLayer = nullptr;
      sPlotCap = nullptr;
      sPlotDrawn = false;
    }
  }, LV_EVENT_DELETE, t);
  plotRefresh(nullptr);
  push(scr);
#else
  lv_label_set_text(cap, "This build has no position receiver.");
  push(Ui::screenOf(body));
#endif
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
