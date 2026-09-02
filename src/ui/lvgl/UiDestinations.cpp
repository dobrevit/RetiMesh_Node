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

namespace {

constexpr size_t kMax = 24;
RnsTransport::PathInfo sPaths[kMax];
size_t sCount = 0;

bool hexToBytes(const char* hex, uint8_t out[16]) {
  if (strlen(hex) < 32) return false;
  for (int i = 0; i < 16; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

void ageText(uint32_t s, char* out, size_t n) {
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

lv_obj_t* reading(lv_obj_t* parent, const char* label, const char* value) {
  lv_obj_t* row = lv_obj_create(parent);
  UiTheme::card(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(row, 8, 0);
  lv_obj_set_style_pad_ver(row, 5, 0);
  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, label);
  UiTheme::labelCaps(l);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* v = lv_label_create(row);
  UiTheme::value(v);
  lv_label_set_text(v, value);
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

void openDetail(lv_event_t* e) {
  const RnsTransport::PathInfo* pi =
      (const RnsTransport::PathInfo*)lv_event_get_user_data(e);
  Neighbor nb = {};
  const bool heard = neighbors.byHash(pi->hash, nb) && nb.name[0];
  char title[34];
  snprintf(title, sizeof(title), "%s", heard ? nb.name : "");
  if (!title[0]) snprintf(title, sizeof(title), "%.8s", pi->hash);
  lv_obj_t* body = Ui::newScreen(title);

  // The full hash, grouped in fours across two lines — the spec's rule: it
  // exists to be verified out loud against another device.
  lv_obj_t* hash = lv_label_create(body);
  char grouped[48];
  size_t w = 0;
  for (size_t i = 0; i < 32 && pi->hash[i] && w < sizeof(grouped) - 3; i++) {
    grouped[w++] = pi->hash[i];
    if ((i % 4) == 3) grouped[w++] = (i == 15) ? '\n' : ' ';
  }
  grouped[w] = 0;
  lv_label_set_text(hash, grouped);
  lv_obj_set_style_text_color(hash, lv_color_hex(UiTheme::kInkDim), 0);

  char v[40];
  snprintf(v, sizeof(v), "%u", pi->hops);
  reading(body, "HOPS", v);
  ageText(pi->ageS, v, sizeof(v));
  char av[44]; snprintf(av, sizeof(av), "%s ago", v);
  reading(body, "LAST HEARD", av);
  reading(body, "VIA", pi->via);
  // The signal rows the spec drew, filled only when this node truly heard
  // the peer itself — a announce that came over RF carries its own figures.
  if (neighbors.byHash(pi->hash, nb) && !nb.viaWifi && nb.rssi != 0) {
    snprintf(v, sizeof(v), "%.0f dBm", (double)nb.rssi);
    reading(body, "LAST RSSI", v);
    snprintf(v, sizeof(v), "%.1f dB", (double)nb.snr);
    reading(body, "SNR", v);
  }
  snprintf(v, sizeof(v), "self -> %s -> %.8s", pi->via, pi->hash);
  lv_obj_t* path = reading(body, "PATH", v);
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
    if (hexToBytes(p2->hash, dest)) Ui::openThread(dest);
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
    ageText(sPaths[i].ageS, age, sizeof(age));
    // The peer's name leads when an announce carried one; the shortened key
    // id sits beneath it either way — the name is for people, the hash is
    // what the mesh actually routes on.
    Neighbor nb = {};
    const bool named = neighbors.byHash(sPaths[i].hash, nb) && nb.name[0];
    snprintf(line, sizeof(line), "%s%s%.8s · %u hop%s · %s",
             named ? nb.name : "", named ? "\n" : "", sPaths[i].hash,
             sPaths[i].hops, sPaths[i].hops == 1 ? "" : "s", age);
    lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_SHUFFLE, line);
    // Stale peers stay listed but recede — quiet an hour is still a fact.
    if (sPaths[i].ageS > 3600)
      lv_obj_set_style_text_color(btn, lv_color_hex(UiTheme::kInkLabel), 0);
    lv_obj_add_event_cb(btn, openDetail, LV_EVENT_CLICKED, &sPaths[i]);
  }

  lv_obj_t* ann = lv_button_create(body);
  UiTheme::actionButton(ann);
  lv_obj_set_width(ann, lv_pct(100));
  lv_obj_set_height(ann, 38);
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
