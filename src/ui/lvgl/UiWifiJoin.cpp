// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiWifiJoin.cpp — joining a network the way a phone does
//
//  Scan, pick, key if the lock icon says so, and save only after the join has
//  actually worked — a password proven wrong on air is a password the store
//  never learns. WifiManager owns the radio side (scan, attempt, persist);
//  this screen owns nothing but the asking.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "WifiManager.h"

namespace {

lv_obj_t* sList   = nullptr;             // the networks, or a line saying why not
lv_obj_t* sStatus = nullptr;             // scanning / joining / verdicts
bool      sScanPending = false;
char      sPickedSsid[33] = "";

void status(const char* text) {
  if (sStatus && lv_obj_is_valid(sStatus)) lv_label_set_text(sStatus, text);
}

void join(const char* password) {
  if (!wifiManager.staJoin(sPickedSsid, password)) {
    Ui::toast("cannot use these credentials (too long?)");
    return;
  }
  char line[64];
  snprintf(line, sizeof(line), "Joining %s...", sPickedSsid);
  status(line);
}

// The password question, asked only when the network's beacon says secured.
void askKey(lv_event_t*) {
  lv_obj_t* box = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(box, sPickedSsid);
  lv_msgbox_add_close_button(box);
  lv_obj_t* body = lv_msgbox_get_content(box);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* ta = Ui::textarea(body, "password", true, false);
  lv_textarea_set_password_mode(ta, true);
  lv_obj_t* btn = lv_button_create(body);
  lv_obj_set_width(btn, lv_pct(100));
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "Join");
  lv_obj_center(bl);
  struct Ctx { lv_obj_t* box; lv_obj_t* ta; };
  static Ctx ctx;                        // one dialog at a time, like the menu
  ctx = { box, ta };
  lv_obj_add_event_cb(btn, [](lv_event_t* e) {
    Ctx* c = (Ctx*)lv_event_get_user_data(e);
    char pass[65];
    snprintf(pass, sizeof(pass), "%s", lv_textarea_get_text(c->ta));
    lv_msgbox_close(c->box);
    join(pass);
  }, LV_EVENT_CLICKED, &ctx);
}

// A hidden network never appears in the scan, so it is asked for by name —
// the one case the old free-text settings rows existed for.
void askHidden(lv_event_t*) {
  if (!wifiManager.wifiEnabled()) { Ui::toast("WiFi is off"); return; }
  lv_obj_t* box = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(box, "Hidden network");
  lv_msgbox_add_close_button(box);
  lv_obj_t* body = lv_msgbox_get_content(box);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* taSsid = Ui::textarea(body, "SSID", true, false);
  lv_obj_t* taPass = Ui::textarea(body, "password (empty if open)", true, false);
  lv_textarea_set_password_mode(taPass, true);
  lv_obj_t* btn = lv_button_create(body);
  lv_obj_set_width(btn, lv_pct(100));
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "Join");
  lv_obj_center(bl);
  struct Ctx { lv_obj_t* box; lv_obj_t* ssid; lv_obj_t* pass; };
  static Ctx ctx;                        // one dialog at a time, like the rest
  ctx = { box, taSsid, taPass };
  lv_obj_add_event_cb(btn, [](lv_event_t* e) {
    Ctx* c = (Ctx*)lv_event_get_user_data(e);
    snprintf(sPickedSsid, sizeof(sPickedSsid), "%s", lv_textarea_get_text(c->ssid));
    char pass[65];
    snprintf(pass, sizeof(pass), "%s", lv_textarea_get_text(c->pass));
    lv_msgbox_close(c->box);
    if (sPickedSsid[0]) join(pass);
  }, LV_EVENT_CLICKED, &ctx);
}

void picked(lv_event_t* e) {
  const bool secured = (bool)(uintptr_t)lv_event_get_user_data(e);
  lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
  // The list button's text is "SSID\n-60 dBm ..." — the first line is the name.
  const char* text = lv_list_get_button_text(sList, btn);
  size_t n = 0;
  while (text[n] && text[n] != '\n' && n < sizeof(sPickedSsid) - 1) n++;
  snprintf(sPickedSsid, sizeof(sPickedSsid), "%.*s", (int)n, text);
  if (secured) askKey(nullptr);
  else join("");
}

void showResults(int count) {
  lv_obj_clean(sList);
  if (count <= 0) {
    lv_list_add_text(sList, "Nothing on the air here.");
    status("No networks found");
    return;
  }
  status("Pick a network");
  for (int i = 0; i < count; i++) {
    WifiManager::StaScanEntry e;
    if (!wifiManager.staScanResult(i, e)) continue;   // hidden names scan empty
    char line[64];
    snprintf(line, sizeof(line), "%s\n%d dBm %s", e.ssid, e.rssi,
             e.secured ? LV_SYMBOL_EYE_CLOSE : "open");
    lv_obj_t* btn = lv_list_add_button(sList, LV_SYMBOL_WIFI, line);
    lv_obj_add_event_cb(btn, picked, LV_EVENT_CLICKED, (void*)(uintptr_t)e.secured);
  }
  wifiManager.staScanDone();
}

// One slow heartbeat drives the whole screen: scan completion and the join
// verdict both arrive by polling, which is all the radio side offers.
void heartbeat(lv_timer_t*) {
  if (!sList || !lv_obj_is_valid(sList)) return;
  if (sScanPending) {
    const int n = wifiManager.staScanCount();
    if (n >= 0) { sScanPending = false; showResults(n); }
  }
  switch (wifiManager.staJoinState()) {
    case WifiManager::StaJoin::Joined:
      Ui::toast("Joined and saved");
      Ui::back();
      break;
    case WifiManager::StaJoin::Failed:
      status("Could not join — wrong key, or the network went away");
      break;
    default: break;
  }
}

void rescan(lv_event_t*) {
  if (!wifiManager.wifiEnabled()) { Ui::toast("WiFi is off"); return; }
  lv_obj_clean(sList);
  status("Scanning...");
  wifiManager.staScanStart();
  sScanPending = true;
}

} // namespace

namespace Ui {

void openWifiJoin() {
  lv_obj_t* body = newScreen("Join Wi-Fi");

  sStatus = lv_label_create(body);
  lv_obj_set_width(sStatus, lv_pct(100));
  lv_label_set_long_mode(sStatus, LV_LABEL_LONG_WRAP);

  sList = lv_list_create(body);
  lv_obj_set_width(sList, lv_pct(100));
  lv_obj_set_flex_grow(sList, 1);

  lv_obj_t* again = lv_button_create(body);
  lv_obj_set_width(again, lv_pct(100));
  lv_obj_t* al = lv_label_create(again);
  lv_label_set_text(al, LV_SYMBOL_REFRESH "  Scan again");
  lv_obj_center(al);
  lv_obj_add_event_cb(again, rescan, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* hidden = lv_button_create(body);
  lv_obj_set_width(hidden, lv_pct(100));
  lv_obj_t* hl = lv_label_create(hidden);
  lv_label_set_text(hl, LV_SYMBOL_EYE_CLOSE "  Join hidden network...");
  lv_obj_center(hl);
  lv_obj_add_event_cb(hidden, askHidden, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  lv_timer_t* t = lv_timer_create(heartbeat, 250, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sList = nullptr; sStatus = nullptr;  // the timer may already be mid-tick
    // An abandoned scan's results are freed rather than parked on a heap
    // this board runs tight; the pending flag dies with the screen.
    if (sScanPending) { wifiManager.staScanDone(); sScanPending = false; }
  }, LV_EVENT_DELETE, t);

  if (!wifiManager.wifiEnabled()) {
    lv_list_add_text(sList, "Wi-Fi is off — switch it on in settings first.");
    status("Wi-Fi is off");
  } else {
    rescan(nullptr);
  }
  push(scr);
}

} // namespace Ui

#endif // HAS_LVGL_UI
