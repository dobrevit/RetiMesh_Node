// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiDestinations.cpp — the mesh, by hops and freshness
//
//  Every destination the transport can route to, nearest and freshest first,
//  stale ones dimmed but kept — a peer that went quiet an hour ago is still
//  a fact worth seeing. The detail screen shows only what this node truly
//  knows about a peer: the path, its length, and its age — never an invented
//  signal figure. MESSAGE drops straight into the conversation; ANNOUNCE
//  asks the schedule for the next slot it allows.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"
#include "RnsTransport.h"
#include "Neighbors.h"
#include "RnsAnnounce.h"
#include "PeerPositions.h"
#include "GeoMath.h"
#include "Gps.h"

namespace {

constexpr size_t kMax = 24;
RnsTransport::PathInfo sPaths[kMax];
size_t sCount = 0;


void openDetail(lv_event_t* e) {
  const RnsTransport::PathInfo* pi =
      (const RnsTransport::PathInfo*)lv_event_get_user_data(e);
  char title[34];
  Ui::peerLabelHex(pi->hash, title, sizeof(title));
  lv_obj_t* body = Ui::newScreen(title);

  // The full hash, grouped in fours across two lines — the spec's rule: it
  // exists to be verified out loud against another device.
  lv_obj_t* hash = lv_label_create(body);
  char grouped[48];
  Ui::groupedHash(pi->hash, grouped, sizeof(grouped));
  lv_label_set_text(hash, grouped);
  lv_obj_set_style_text_color(hash, lv_color_hex(UiTheme::kInkDim), 0);

  char v[40];
  snprintf(v, sizeof(v), "%u", pi->hops);
  UiTheme::reading(body, "HOPS", v);
  Ui::ageTextS(pi->ageS, v, sizeof(v));
  char av[44]; snprintf(av, sizeof(av), "%s ago", v);
  UiTheme::reading(body, "LAST HEARD", av);
  UiTheme::reading(body, "VIA", pi->via);
  // The signal rows the spec drew, filled only when this node truly heard
  // the peer itself — an announce that came over RF carries its own figures.
  Neighbor nb = {};
  if (neighbors.byHash(pi->hash, nb) && !nb.viaWifi && nb.rssi != 0) {
    snprintf(v, sizeof(v), "%.0f dBm", (double)nb.rssi);
    UiTheme::reading(body, "LAST RSSI", v);
    snprintf(v, sizeof(v), "%.1f dB", (double)nb.snr);
    UiTheme::reading(body, "SNR", v);
  }
#if HAS_GPS
  {
    // The peer's announced position is the peer's fact — shown whenever it
    // exists. The geometry rows need our end of the baseline too, so they
    // appear only with a fix; hiding the peer's fact behind our fix once
    // made an arrived position invisible on a node standing indoors.
    PeerPositions::Position pp;
    if (PeerPositions::getByHex(pi->hash, pp)) {
      char age[8];
      Ui::ageTextMs(millis() - pp.heardMs, age, sizeof(age));
      snprintf(v, sizeof(v), "%s ago · ±%.0f m", age, (double)pp.accuracyM);
      UiTheme::reading(body, "POS HEARD", v);
      const Gps::Fix own = Gps::fix();
      if (own.valid) {
        double km, deg;
        GeoMath::distanceAndBearing(own.latitude, own.longitude,
                                    pp.latitude, pp.longitude, km, deg);
        Ui::formatKm(v, sizeof(v), km);
        UiTheme::reading(body, "DISTANCE", v);
        snprintf(v, sizeof(v), "%.0f°", deg);
        UiTheme::reading(body, "BEARING", v);
      }
    }
  }
#endif
  snprintf(v, sizeof(v), "self -> %s -> %.8s", pi->via, pi->hash);
  lv_obj_t* path = UiTheme::reading(body, "PATH", v);
  lv_obj_set_style_text_color(path, lv_color_hex(UiTheme::kInkDim), 0);

  lv_obj_t* row = lv_obj_create(body);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 42);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_t* msg = lv_button_create(row);
  UiTheme::actionButton(msg);
  lv_obj_set_flex_grow(msg, 1);
  lv_obj_set_height(msg, lv_pct(100));
  lv_obj_t* ml = lv_label_create(msg);
  lv_label_set_text(ml, "MESSAGE");
  lv_obj_center(ml);
  lv_obj_add_event_cb(msg, [](lv_event_t* ev) {
    const RnsTransport::PathInfo* p2 =
        (const RnsTransport::PathInfo*)lv_event_get_user_data(ev);
    uint8_t dest[16];
    if (Rns::hexToBytes16(p2->hash, dest)) Ui::openThread(dest);
  }, LV_EVENT_CLICKED, (void*)pi);
  lv_obj_t* nav = lv_button_create(row);
  UiTheme::actionButton(nav);
  lv_obj_set_flex_grow(nav, 1);
  lv_obj_set_height(nav, lv_pct(100));
  lv_obj_t* nl = lv_label_create(nav);
  lv_label_set_text(nl, "NAV");
  lv_obj_center(nl);
  lv_obj_add_event_cb(nav, [](lv_event_t* ev) {
    const RnsTransport::PathInfo* p2 =
        (const RnsTransport::PathInfo*)lv_event_get_user_data(ev);
    char t[34];
    Ui::peerLabelHex(p2->hash, t, sizeof(t));
    Ui::openBearing(t, p2->hash);
  }, LV_EVENT_CLICKED, (void*)pi);
  lv_obj_t* ann = lv_button_create(row);
  UiTheme::actionButton(ann);
  lv_obj_set_flex_grow(ann, 1);
  lv_obj_set_height(ann, lv_pct(100));
  lv_obj_t* al = lv_label_create(ann);
  lv_label_set_text(al, "ANNOUNCE");
  lv_obj_center(al);
  lv_obj_add_event_cb(ann, [](lv_event_t*) {
    RnsTransport::announceNow();
    Ui::toast("announce asked for");
  }, LV_EVENT_CLICKED, nullptr);

  Ui::push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace

namespace Ui {

void openDestinations() {
  lv_obj_t* body = newScreen("Mesh");

  sCount = RnsTransport::paths(sPaths, kMax);
  // Nearest and freshest first, the order a person routes by.
  for (size_t i = 0; i + 1 < sCount; i++)
    for (size_t j = 0; j + 1 < sCount - i; j++) {
      const bool swap = sPaths[j].hops > sPaths[j + 1].hops ||
                        (sPaths[j].hops == sPaths[j + 1].hops &&
                         sPaths[j].ageS > sPaths[j + 1].ageS);
      if (swap) { auto t = sPaths[j]; sPaths[j] = sPaths[j + 1]; sPaths[j + 1] = t; }
    }

  lv_obj_t* list = lv_list_create(body);
  lv_obj_set_width(list, lv_pct(100));
  lv_obj_set_flex_grow(list, 1);
  if (!sCount) lv_list_add_text(list, "nothing announced yet");
  for (size_t i = 0; i < sCount; i++) {
    char age[8], line[80];
    Ui::ageTextS(sPaths[i].ageS, age, sizeof(age));
    // The peer's name leads when an announce carried one; the shortened key
    // id sits beneath it either way — the name is for people, the hash is
    // what the mesh actually routes on.
    char who[34];
    Ui::peerLabelHex(sPaths[i].hash, who, sizeof(who));
    const bool named = strncmp(who, sPaths[i].hash, 8) != 0;
    snprintf(line, sizeof(line), "%s%s%.8s · %u hop%s · %s",
             named ? who : "", named ? "\n" : "", sPaths[i].hash,
             sPaths[i].hops, sPaths[i].hops == 1 ? "" : "s", age);
    lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_SHUFFLE, line);
    // Stale peers stay listed but recede — quiet an hour is still a fact.
    if (sPaths[i].ageS > 3600)
      lv_obj_set_style_text_color(btn, lv_color_hex(UiTheme::kInkLabel), 0);
    lv_obj_add_event_cb(btn, openDetail, LV_EVENT_CLICKED, &sPaths[i]);
  }

  lv_obj_t* foot = lv_obj_create(body);
  lv_obj_remove_style_all(foot);
  lv_obj_set_size(foot, lv_pct(100), 38);
  lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(foot, 6, 0);
  lv_obj_t* map = lv_button_create(foot);
  UiTheme::actionButton(map);
  lv_obj_set_flex_grow(map, 1);
  lv_obj_set_height(map, lv_pct(100));
  lv_obj_t* mpl = lv_label_create(map);
  lv_label_set_text(mpl, "MAP");
  lv_obj_center(mpl);
  lv_obj_add_event_cb(map, [](lv_event_t*) { Ui::openPlot(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ann = lv_button_create(foot);
  UiTheme::actionButton(ann);
  lv_obj_set_flex_grow(ann, 2);
  lv_obj_set_height(ann, lv_pct(100));
  lv_obj_t* al = lv_label_create(ann);
  lv_label_set_text(al, LV_SYMBOL_UPLOAD "  ANNOUNCE THIS NODE");
  lv_obj_center(al);
  lv_obj_add_event_cb(ann, [](lv_event_t*) {
    RnsTransport::announceNow();
    Ui::toast("announce asked for");
  }, LV_EVENT_CLICKED, nullptr);

  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace Ui

#endif // HAS_LVGL_UI
