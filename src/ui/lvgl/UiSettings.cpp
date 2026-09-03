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
//  UiSettings.cpp — categories, forms, and Cancel / Save
//
//  The category list is the sections of the console's own key table; a
//  category opens a form whose controls are generated from what each key
//  renders — a switch where it says on/off, a dropdown where a canonical
//  word list exists (regions from Airtime, security and power profile from
//  their name helpers), a numeric pad where the value is a number, and the
//  keyboard for the rest. Nothing is applied while the operator edits:
//  Save walks the changed controls through SettingsFields::set — the same
//  validation, wording and apply-live behaviour as SET at the console — and
//  Cancel walks away. A key added to the table next month appears here
//  without this file changing.
// ============================================================================
#include "Ui.h"
#include "UiTheme.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "SettingsFields.h"
#include "Settings.h"
#include "Power.h"
#include "Airtime.h"
#include "Bootloader.h"
#include "PeerNames.h"
#include "LxmfInbox.h"
#include "WifiManager.h"

namespace {

// --- what kind of control a key wants ---------------------------------------

enum class Kind : uint8_t { Text, Secret, Number, Switch, Region, Words, Slider };

Kind kindFor(const char* key, const char* value, bool quoted) {
  if (strcmp(key, "display.brightness") == 0)       return Kind::Slider;
  if (strcmp(key, "radio.region") == 0)             return Kind::Region;
  if (strcmp(key, "wifi.security") == 0)            return Kind::Words;
  if (strcmp(key, "display.theme") == 0)            return Kind::Words;
  if (strcmp(key, "transport.power_profile") == 0)  return Kind::Words;
  // Quoted is the funnel's own convention for free text (renderStr quotes,
  // bools and numbers render bare) — so an SSID that happens to read "off"
  // or "12345678" is text, and the sniffing below never sees it.
  if (quoted) return Kind::Text;
  if (strcmp(value, "on") == 0 || strcmp(value, "off") == 0) return Kind::Switch;
  if (strcmp(value, "(set)") == 0 || strcmp(value, "(unset)") == 0) return Kind::Secret;
  // A number, possibly signed, possibly with a decimal point.
  const char* p = value;
  if (*p == '-') p++;
  bool digits = false, dot = false, numeric = *p != 0;
  for (; *p && numeric; p++) {
    if (*p >= '0' && *p <= '9') digits = true;
    else if (*p == '.' && !dot) dot = true;
    else numeric = false;
  }
  if (numeric && digits) return Kind::Number;
  return Kind::Text;
}

// The dropdown's words, from the enums' own name helpers — retyped lists
// went silently stale the day the enum grew.
void wordsFor(const char* key, char* out, size_t len) {
  size_t n = 0;
  if (strcmp(key, "display.theme") == 0) {
    snprintf(out, len, "night\nday");
    return;
  }
  if (strcmp(key, "wifi.security") == 0) {
    for (uint8_t i = 0; i <= (uint8_t)ApSecurity::WPA3; i++)
      n += snprintf(out + n, len - n, "%s%s", i ? "\n" : "",
                    Settings::securityName((ApSecurity)i));
  } else {
    for (uint8_t i = 0; i <= (uint8_t)Power::Profile::Battery; i++)
      n += snprintf(out + n, len - n, "%s%s", i ? "\n" : "",
                    Power::profileName((Power::Profile)i));
  }
}

// --- one form row ------------------------------------------------------------

// A row remembers what it started as, so Save can skip what did not change
// and Cancel can simply leave.
struct Row {
  char      key[40];
  // Sized for the longest rendered value — the admins list runs to 139
  // characters, and a 96-byte draft showed it cut mid-hash and saved the
  // fragment back.
  char      initial[160];
  Kind      kind;
  lv_obj_t* control;
  bool      used;
};
constexpr size_t kMaxRows = 20;
Row sRows[kMaxRows];

// The value a control holds now, in the words SettingsFields::set eats.
void rowValue(const Row& r, char* out, size_t len) {
  switch (r.kind) {
    case Kind::Slider:
      snprintf(out, len, "%d", (int)lv_slider_get_value(r.control));
      break;
    case Kind::Switch:
      snprintf(out, len, "%s", lv_obj_has_state(r.control, LV_STATE_CHECKED) ? "on" : "off");
      break;
    case Kind::Region:
    case Kind::Words: {
      char sel[48];
      lv_dropdown_get_selected_str(r.control, sel, sizeof(sel));
      if (r.kind == Kind::Region) {
        // The dropdown shows the human name; the funnel takes the key.
        size_t n = 0;
        const Airtime::RegionInfo* regions = Airtime::regions(n);
        for (size_t i = 0; i < n; i++)
          if (strcmp(regions[i].name, sel) == 0) { snprintf(out, len, "%s", regions[i].key); return; }
      }
      snprintf(out, len, "%s", sel);
      break;
    }
    default:
      snprintf(out, len, "%s", lv_textarea_get_text(r.control));
      break;
  }
}

bool rowChanged(const Row& r) {
  char now[160];
  rowValue(r, now, sizeof(now));
  if (r.kind == Kind::Secret) return now[0] != 0;      // typed at all = change
  return strcmp(now, r.initial) != 0;
}

// A slider's value into the funnel, and the row's baseline moved with it so
// Save has nothing left to do.
void applySliderRow(Row* row) {
  char v[8], err[96] = "";
  snprintf(v, sizeof(v), "%d", (int)lv_slider_get_value(row->control));
  const SettingsFields::Result res =
      SettingsFields::set(row->key, v, err, sizeof(err), Bootloader::Source::Ui);
  if (res == SettingsFields::Result::Ok)
    strlcpy(row->initial, v, sizeof(row->initial));
  else Ui::toast(err[0] ? err : SettingsFields::resultText(res));
}

void buildControl(lv_obj_t* parent, Row& r) {
  switch (r.kind) {
    case Kind::Switch: {
      r.control = lv_switch_create(parent);
      if (strcmp(r.initial, "on") == 0) lv_obj_add_state(r.control, LV_STATE_CHECKED);
      break;
    }
    case Kind::Region: {
      r.control = lv_dropdown_create(parent);
      size_t n = 0;
      const Airtime::RegionInfo* regions = Airtime::regions(n);
      char opts[512];
      size_t o = 0;
      uint32_t sel = 0;
      for (size_t i = 0; i < n && o + 40 < sizeof(opts); i++) {
        o += snprintf(opts + o, sizeof(opts) - o, "%s%s", i ? "\n" : "", regions[i].name);
        if (strcmp(regions[i].key, r.initial) == 0) sel = (uint32_t)i;
      }
      lv_dropdown_set_options(r.control, opts);
      lv_dropdown_set_selected(r.control, sel);
      lv_obj_set_width(r.control, lv_pct(100));
      break;
    }
    case Kind::Words: {
      r.control = lv_dropdown_create(parent);
      char words[64];
      wordsFor(r.key, words, sizeof(words));
      lv_dropdown_set_options(r.control, words);
      // Select the current value by walking the option lines.
      uint32_t idx = 0, sel = 0;
      for (const char* p = words; *p; idx++) {
        const char* e = strchr(p, '\n');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strncmp(p, r.initial, n) == 0 && r.initial[n] == 0) sel = idx;
        if (!e) break;
        p = e + 1;
      }
      lv_dropdown_set_selected(r.control, sel);
      lv_obj_set_width(r.control, lv_pct(100));
      break;
    }
    case Kind::Slider: {
      // Typing a percentage is tedious on glass: a slider with step keys on
      // either side, applied through the funnel the moment the finger lifts,
      // so the backlight answers while the hand is still on the panel.
      lv_obj_t* rowH = lv_obj_create(parent);
      lv_obj_remove_style_all(rowH);
      lv_obj_set_size(rowH, lv_pct(100), 36);
      lv_obj_set_flex_flow(rowH, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(rowH, 8, 0);
      lv_obj_set_flex_align(rowH, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      auto stepBtn = [&](const char* sym) {
        lv_obj_t* b = lv_button_create(rowH);
        UiTheme::actionButton(b);
        lv_obj_set_size(b, 40, lv_pct(100));
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, sym);
        lv_obj_center(l);
        return b;
      };
      lv_obj_t* minus = stepBtn(LV_SYMBOL_MINUS);
      lv_obj_t* slider = lv_slider_create(rowH);
      // The floor is BRIGHTNESS_FLOOR_PCT everywhere — funnel, load, and
      // this slider ask the same constant.
      lv_slider_set_range(slider, BRIGHTNESS_FLOOR_PCT, 100);
      const int init = atoi(r.initial);
      lv_slider_set_value(slider, init < BRIGHTNESS_FLOOR_PCT ? BRIGHTNESS_FLOOR_PCT : init, LV_ANIM_OFF);
      lv_obj_set_flex_grow(slider, 1);
      lv_obj_t* plus = stepBtn(LV_SYMBOL_PLUS);
      r.control = slider;
      // Applied by direct call, never by a synthetic event: sending the
      // slider a fake RELEASED made its class handler re-derive the value
      // from the touch point's last position — the step key itself, at the
      // screen's edge — and one tap on minus blacked the glass.
      lv_obj_add_event_cb(slider, [](lv_event_t* e) {
        applySliderRow((Row*)lv_event_get_user_data(e));
      }, LV_EVENT_RELEASED, &r);
      struct Step { Row* row; int delta; };
      static Step steps[kMaxRows * 2];
      static size_t stepN = 0;
      if (stepN + 2 > kMaxRows * 2) stepN = 0;
      steps[stepN] = { &r, -5 };
      steps[stepN + 1] = { &r, +5 };
      auto stepCb = [](lv_event_t* e) {
        Step* st = (Step*)lv_event_get_user_data(e);
        int v = (int)lv_slider_get_value(st->row->control) + st->delta;
        v = v < BRIGHTNESS_FLOOR_PCT ? BRIGHTNESS_FLOOR_PCT : (v > 100 ? 100 : v);
        lv_slider_set_value(st->row->control, v, LV_ANIM_OFF);
        applySliderRow(st->row);
      };
      lv_obj_add_event_cb(minus, stepCb, LV_EVENT_CLICKED, &steps[stepN]);
      lv_obj_add_event_cb(plus, stepCb, LV_EVENT_CLICKED, &steps[stepN + 1]);
      stepN += 2;
      break;
    }
    case Kind::Secret:
      r.control = Ui::textarea(parent, "unchanged — type to replace", true, false);
      lv_textarea_set_password_mode(r.control, true);
      break;
    case Kind::Number:
      r.control = Ui::textarea(parent, "", true, true);
      lv_textarea_set_text(r.control, r.initial);
      break;
    default:
      r.control = Ui::textarea(parent, "", true, false);
      lv_textarea_set_text(r.control, r.initial);
      break;
  }
}

// --- the form ---------------------------------------------------------------

void saveForm(lv_event_t*) {
  size_t changed = 0, failed = 0;
  char firstErr[128] = "";
  // One gesture, one restart: without the batch the first changed Wi-Fi key
  // armed it and every later key was refused Busy — half a form applied,
  // then a reboot with the rest unsaved.
  SettingsFields::beginBatch();
  for (Row& r : sRows) {
    if (!r.used || !rowChanged(r)) continue;
    char value[160], err[128] = "";
    rowValue(r, value, sizeof(value));
    const SettingsFields::Result res =
        SettingsFields::set(r.key, value, err, sizeof(err), Bootloader::Source::Ui);
    const bool ok = res == SettingsFields::Result::Ok ||
                    res == SettingsFields::Result::OkRestart ||
                    res == SettingsFields::Result::OkNextBoot;
    if (ok) changed++;
    else {
      failed++;
      if (!firstErr[0])
        snprintf(firstErr, sizeof(firstErr), "%s: %s", r.key,
                 err[0] ? err : SettingsFields::resultText(res));
    }
  }
  SettingsFields::endBatch();          // arms the one restart, if any key asked
  char line[160];
  if (failed) snprintf(line, sizeof(line), "%u saved, %u refused — %s",
                       (unsigned)changed, (unsigned)failed, firstErr);
  else if (changed) snprintf(line, sizeof(line), "%u setting%s saved",
                             (unsigned)changed, changed == 1 ? "" : "s");
  else snprintf(line, sizeof(line), "nothing changed");
  Ui::toast(line);
  if (!failed) Ui::back();               // a refusal keeps the form for fixing
}

// The radio form's consequence line: every parameter change costs range or
// airtime, so the cost is printed under the controls and follows the staged
// values — what the operator is about to APPLY, not what the node runs.
// Owner-checked like UiNav's dial: back()'s deferred 150 ms delete must
// never null a freshly opened page's statics.
lv_obj_t* sRadioFoot  = nullptr;
lv_obj_t* sRadioOwner = nullptr;

const Row* rowFor(const char* key) {
  for (const Row& r : sRows)
    if (r.used && strcmp(r.key, key) == 0) return &r;
  return nullptr;
}

void radioFootTick(lv_timer_t*) {
  if (!sRadioFoot || !lv_obj_is_valid(sRadioFoot)) return;
  char v[24];
  Airtime::Params p;
  int8_t dbm = settings.radio().txDbm;
  // All three shaping rows must be this page's own: Params' defaults pass
  // the range check below, so a missing row — another section's rows in
  // the arena — would print a confident estimate nobody chose.
  const Row* rsf = rowFor("radio.sf");
  const Row* rbw = rowFor("radio.bw_khz");
  const Row* rcr = rowFor("radio.cr");
  if (!rsf || !rbw || !rcr) return;
  rowValue(*rsf, v, sizeof(v)); p.sf = (uint8_t)atoi(v);
  rowValue(*rbw, v, sizeof(v)); p.bwKhz = (float)atof(v);
  rowValue(*rcr, v, sizeof(v)); p.cr = (uint8_t)atoi(v);
  if (const Row* r = rowFor("radio.tx_dbm")) { rowValue(*r, v, sizeof(v)); dbm = (int8_t)atoi(v); }
  if (p.sf < 5 || p.sf > 12 || p.bwKhz < 7.0f || p.cr < 5 || p.cr > 8) return;  // mid-edit
  Airtime a;
  a.configure(p);
  const float ms = a.timeOnAirMs(200);
  // A deliberately rough line-of-sight guess, and labelled as one: SF9/125
  // as 4 km, a third more per SF step, narrower bandwidth buying a little,
  // a dB of power a few percent. It ranks choices; it does not promise.
  const float km = 4.0f * powf(1.35f, (float)p.sf - 9.0f) *
                   sqrtf(125.0f / p.bwKhz) * powf(1.04f, (float)dbm - 17.0f);
  char line[96];
  snprintf(line, sizeof(line), "est. range ~%.1f km · %.2f s per 200 B\nduty used %.1f%% of %.1f%%",
           (double)km, (double)(ms / 1000.0f),
           (double)(g_stats.airtimeLong * 100.0f), (double)(g_stats.dutyLimitBp / 100.0f));
  if (strcmp(lv_label_get_text(sRadioFoot), line)) lv_label_set_text(sRadioFoot, line);
}

// The station's line on the wifi page, kept live while the page is open,
// with the same owner rule as the radio foot above.
lv_obj_t* sStaStatus = nullptr;
lv_obj_t* sStaOwner  = nullptr;

void staStatusTick(lv_timer_t*) {
  if (!sStaStatus || !lv_obj_is_valid(sStaStatus)) return;
  const char* ssid = settings.wifi().staSsid;
  char line[96];
  if (!settings.links().wifiEnabled) {
    // The truth outranks the saved network: with the adapter off nothing is
    // looking for anything — a "searching" line here once claimed a hunt the
    // driver was never even started for.
    snprintf(line, sizeof(line), "WiFi is off — the switch above turns it on.");
  } else if (!ssid[0]) {
    snprintf(line, sizeof(line), "No network saved — scan to join one.");
  } else if (wifiManager.stationConnected()) {
    char ip[20];
    wifiManager.staIpText(ip, sizeof(ip));
    snprintf(line, sizeof(line), "Connected to %s\n%s, %d dBm", ssid, ip, wifiManager.staRssi());
  } else {
    snprintf(line, sizeof(line), "Looking for %s...", ssid);
  }
  if (strcmp(lv_label_get_text(sStaStatus), line)) lv_label_set_text(sStaStatus, line);
}

// The wifi page leads with the station: what it is doing right now, the
// scanner to change it, and — while a network is saved — the way out. The
// raw sta_ssid/sta_password rows are gone from the glass (the scanner and
// its hidden-network dialog own joining; the console and portal still
// carry the keys).
void wifiExtras(lv_obj_t* body) {
  // The adapter's master switch rides the status row, as the design draws
  // it — through the funnel, whose link rules refuse a combination that
  // would leave no way into the node.
  lv_obj_t* head = lv_obj_create(body);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  sStaStatus = lv_label_create(head);
  lv_obj_set_width(sStaStatus, lv_pct(74));
  lv_label_set_long_mode(sStaStatus, LV_LABEL_LONG_WRAP);
  lv_obj_align(sStaStatus, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* master = lv_switch_create(head);
  lv_obj_align(master, LV_ALIGN_RIGHT_MID, 0, 0);
  if (settings.links().wifiEnabled) lv_obj_add_state(master, LV_STATE_CHECKED);
  lv_obj_add_event_cb(master, [](lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    const bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);
    char err[128] = "";
    const SettingsFields::Result res =
        SettingsFields::set("links.wifi", want ? "on" : "off", err, sizeof(err),
                            Bootloader::Source::Ui);
    const bool ok = res == SettingsFields::Result::Ok ||
                    res == SettingsFields::Result::OkRestart ||
                    res == SettingsFields::Result::OkNextBoot;
    if (!ok) {
      // The funnel said no (usually the no-way-in rule); the switch tells
      // the truth again and the reason gets the toast.
      if (want) lv_obj_remove_state(sw, LV_STATE_CHECKED);
      else      lv_obj_add_state(sw, LV_STATE_CHECKED);
      Ui::toast(err[0] ? err : SettingsFields::resultText(res));
    } else if (res != SettingsFields::Result::Ok) {
      Ui::toast(SettingsFields::resultText(res));
      // A restart is armed: the funnel now refuses every write, so the
      // form below could be edited but never saved. The page steps aside
      // instead of offering dead controls.
      if (res == SettingsFields::Result::OkRestart) Ui::back();
    }
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_timer_t* t = lv_timer_create(staStatusTick, 1000, nullptr);
  sStaOwner = Ui::screenOf(body);
  lv_obj_add_event_cb(sStaOwner,
                      [](lv_event_t* ev) {
                        lv_timer_delete((lv_timer_t*)lv_event_get_user_data(ev));
                        // The timer always dies with its screen; the
                        // statics only when the dying screen still owns
                        // them — a stale deferred delete must not null a
                        // freshly opened page's.
                        if ((lv_obj_t*)lv_event_get_target(ev) == sStaOwner) {
                          sStaOwner = nullptr;
                          sStaStatus = nullptr;
                        }
                      }, LV_EVENT_DELETE, t);
  staStatusTick(nullptr);

  lv_obj_t* scan = lv_button_create(body);
  lv_obj_set_width(scan, lv_pct(100));
  lv_obj_t* sl = lv_label_create(scan);
  lv_label_set_text(sl, LV_SYMBOL_WIFI "  Join a network...");
  lv_obj_center(sl);
  lv_obj_add_event_cb(scan, [](lv_event_t*) { Ui::openWifiJoin(); }, LV_EVENT_CLICKED, nullptr);
  // Locked, not hidden, while the adapter is off — the master switch's
  // restart rebuilds this page, so build-time state is the truth.
  if (!settings.links().wifiEnabled) lv_obj_add_state(scan, LV_STATE_DISABLED);

  if (wifiManager.stationConfigured()) {
    lv_obj_t* forget = lv_button_create(body);
    lv_obj_set_width(forget, lv_pct(100));
    lv_obj_t* fl = lv_label_create(forget);
    lv_label_set_text(fl, LV_SYMBOL_CLOSE "  Disconnect & forget");
    lv_obj_center(fl);
    if (!settings.links().wifiEnabled) lv_obj_add_state(forget, LV_STATE_DISABLED);
    lv_obj_add_event_cb(forget, [](lv_event_t* ev) {
      wifiManager.staForget();
      Ui::toast("Network forgotten");
      lv_obj_add_state((lv_obj_t*)lv_event_get_target(ev), LV_STATE_DISABLED);
    }, LV_EVENT_CLICKED, nullptr);
  }
}

// wifiExtras' other half: the scanner (and its hidden-SSID dialog) owns
// joining, so the raw credential rows leave the glass — a second,
// unverified way to say the same thing. One decision, one place: removing
// the wifi table row means removing this with it.
bool wifiHidesKey(const char* k) {
  return strcmp(k, "wifi.sta_ssid") == 0 || strcmp(k, "wifi.sta_password") == 0;
}

void radioExtras(lv_obj_t* body) {
  lv_obj_t* card = lv_obj_create(body);
  UiTheme::card(card);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_t* cl = lv_label_create(card);
  lv_label_set_text(cl, "CONSEQUENCE");
  UiTheme::labelCaps(cl);
  sRadioFoot = lv_label_create(card);
  lv_obj_set_width(sRadioFoot, lv_pct(100));
  lv_label_set_long_mode(sRadioFoot, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(sRadioFoot, lv_color_hex(UiTheme::kInkDim), 0);
  lv_label_set_text(sRadioFoot, "");
  lv_obj_align_to(sRadioFoot, cl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);
  lv_timer_t* t = lv_timer_create(radioFootTick, 500, nullptr);
  sRadioOwner = Ui::screenOf(body);
  lv_obj_add_event_cb(sRadioOwner,
                      [](lv_event_t* ev) {
                        lv_timer_delete((lv_timer_t*)lv_event_get_user_data(ev));
                        if ((lv_obj_t*)lv_event_get_target(ev) == sRadioOwner) {
                          sRadioOwner = nullptr;
                          sRadioFoot = nullptr;
                        }
                      }, LV_EVENT_DELETE, t);
  radioFootTick(nullptr);
}

void displayExtras(lv_obj_t* body) {
  // The discharge sparkline: the evidence that a power profile is doing
  // something, one point per five minutes over eight hours.
  uint8_t h[96];
  const size_t hn = Power::batteryHistory(h, 96);
  if (hn >= 2) {
    lv_obj_t* card = lv_obj_create(body);
    UiTheme::card(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 84);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_t* cl = lv_label_create(card);
    lv_label_set_text(cl, "BATTERY · LAST 8 H");
    UiTheme::labelCaps(cl);
    lv_obj_t* chart = lv_chart_create(card);
    lv_obj_set_size(chart, lv_pct(100), 52);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(chart, (uint32_t)hn);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_series_t* ser =
        lv_chart_add_series(chart, lv_color_hex(UiTheme::kGood), LV_CHART_AXIS_PRIMARY_Y);
    for (size_t i = 0; i < hn; i++) lv_chart_set_next_value(chart, ser, h[i]);
  }
}

void maintenanceExtras(lv_obj_t* body) {
  // The one irreversible control, in the one irreversible colour, behind
  // the one gesture gloves and rain cannot fake: a real two-second hold.
  lv_obj_t* erase = lv_button_create(body);
  UiTheme::actionButton(erase);
  lv_obj_set_width(erase, lv_pct(100));
  lv_obj_set_height(erase, 40);
  lv_obj_t* el = lv_label_create(erase);
  lv_label_set_text(el, "ERASE NODE — HOLD 2 s");
  lv_obj_set_style_text_color(el, lv_color_hex(UiTheme::kBad), 0);
  lv_obj_center(el);
  Ui::onHeld2s(erase, [](void*) {
    settings.factoryReset();
    // The control says erase, so the personal data goes with the
    // settings: the stored conversations and the remembered peer names.
    // The RNS identity survives — destroying the key is the dedicated
    // screen below, with its own words.
    Rns::Inbox::wipe();
    PeerNames::wipe();
    Ui::toast("erased — restarting");
    Bootloader::reboot(Bootloader::Source::Ui);
  }, nullptr);

  lv_obj_t* eid = lv_button_create(body);
  UiTheme::actionButton(eid);
  lv_obj_set_width(eid, lv_pct(100));
  lv_obj_set_height(eid, 40);
  lv_obj_t* eil = lv_label_create(eid);
  lv_label_set_text(eil, "ERASE IDENTITY " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(eil, lv_color_hex(UiTheme::kBad), 0);
  lv_obj_center(eil);
  lv_obj_add_event_cb(eid, [](lv_event_t*) { Ui::openEraseIdentity(); },
                      LV_EVENT_CLICKED, nullptr);
}

// Every section's hand-built furniture, keyed by name: what stands above
// the generated rows and what follows them. openCategory consults the
// table instead of growing another strcmp branch — a new section adds a
// row here, not a block there. (Per-row tweaks — transport.wifi_mode's
// lock-and-explain in the row loop — are about one control, not a
// section's furniture, and stay with the row that owns them.)
struct SectionExtras {
  const char* section;
  void (*before)(lv_obj_t* body);        // above the generated rows
  void (*after)(lv_obj_t* body);         // below them
};
constexpr SectionExtras kSectionExtras[] = {
  { "wifi",        wifiExtras, nullptr           },
  { "radio",       nullptr,    radioExtras       },   // after only: reads sRows
  { "display",     nullptr,    displayExtras     },
  { "maintenance", nullptr,    maintenanceExtras },
};

void openCategory(lv_event_t* e) {
  const char* section = (const char*)lv_event_get_user_data(e);
  // A breadcrumb before the heavy build: if this screen ever takes the node
  // down again, the log names the section instead of leaving a silent panic
  // on a UART nobody wired.
  log_i("gui: opening settings/%s", section);
  char title[24];
  snprintf(title, sizeof(title), "%s", section);
  if (title[0] >= 'a' && title[0] <= 'z') title[0] -= 32;
  lv_obj_t* body = Ui::newScreen(title);

  // The arena is cleared before any hook runs: a before-hook that asks
  // rowFor() must see this page's rows (none yet), never the last page's
  // freed controls.
  memset(sRows, 0, sizeof(sRows));
  size_t used = 0;

  // Case-insensitive to match keyInSection: the rows and the furniture
  // must agree on what a section name is.
  const SectionExtras* extras = nullptr;
  for (const SectionExtras& x : kSectionExtras)
    if (strcasecmp(section, x.section) == 0) { extras = &x; break; }
  if (extras && extras->before) extras->before(body);

  for (size_t i = 0; i < SettingsFields::count(); i++) {
    if (!SettingsFields::keyInSection(i, section)) continue;
    const char* k = SettingsFields::keyAt(i);
    if (k && wifiHidesKey(k)) continue;
    if (used >= kMaxRows) {
      // Truncation must be visible: the keys past the cap stay real on
      // the console, and silence here would read as "that setting is
      // gone".
      log_w("gui: settings/%s holds more keys than the form (%u); the rest stay console-only",
            section, (unsigned)kMaxRows);
      break;
    }
    char line[224];
    if (!SettingsFields::render(i, line, sizeof(line))) continue;
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char* value = eq + 1;
    // Strings render quoted; the editor wants the bare value.
    size_t vn = strlen(value);
    const bool quoted = vn >= 2 && value[0] == '"' && value[vn - 1] == '"';
    if (quoted) { value[vn - 1] = 0; value++; }

    Row& r = sRows[used++];
    r.used = true;
    strlcpy(r.key, line, sizeof(r.key));
    strlcpy(r.initial, value, sizeof(r.initial));
    r.kind = kindFor(r.key, value, quoted);

    lv_obj_t* rowBox = lv_obj_create(body);
    lv_obj_remove_style_all(rowBox);
    lv_obj_set_width(rowBox, lv_pct(100));
    lv_obj_set_height(rowBox, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rowBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rowBox, 2, 0);
    lv_obj_t* lbl = lv_label_create(rowBox);
    lv_label_set_text(lbl, strchr(r.key, '.') ? strchr(r.key, '.') + 1 : r.key);
    buildControl(rowBox, r);

    // The transport's WiFi usage depends on the adapter: with the adapter
    // off the control locks rather than pretending, and says where the key
    // to it lives.
    if (strcmp(r.key, "transport.wifi_mode") == 0 && !settings.links().wifiEnabled) {
      lv_obj_add_state(r.control, LV_STATE_DISABLED);
      lv_obj_t* why = lv_label_create(rowBox);
      lv_label_set_text(why, "locked — the WiFi adapter is off (Settings > wifi)");
      UiTheme::labelCaps(why);
    }
  }

  if (extras && extras->after) extras->after(body);

  // Cancel and Save, side by side, the row every form ends with.
  lv_obj_t* btns = lv_obj_create(body);
  lv_obj_remove_style_all(btns);
  lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btns, 6, 0);
  lv_obj_t* cancel = lv_button_create(btns);
  lv_obj_set_flex_grow(cancel, 1);
  lv_obj_t* cl = lv_label_create(cancel);
  lv_label_set_text(cl, "Cancel");
  lv_obj_center(cl);
  lv_obj_add_event_cb(cancel, [](lv_event_t*) { Ui::back(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* save = lv_button_create(btns);
  lv_obj_set_flex_grow(save, 1);
  lv_obj_t* sl = lv_label_create(save);
  lv_label_set_text(sl, "Save");
  lv_obj_center(sl);
  lv_obj_add_event_cb(save, saveForm, LV_EVENT_CLICKED, nullptr);

  Ui::push(Ui::screenOf(body));
}

// The sections, in table order, each once. Static storage because the event
// callbacks keep pointing at them for as long as the list lives.
constexpr size_t kMaxSections = 8;
char sSections[kMaxSections][16];

} // namespace

namespace Ui {

void openSettings() {
  lv_obj_t* body = newScreen("Settings");
  lv_obj_t* list = lv_list_create(body);
  lv_obj_set_width(list, lv_pct(100));
  lv_obj_set_flex_grow(list, 1);

  // The table's names are key prefixes by contract; a renamed section
  // would otherwise leave a silently dead entry, sparkline and all.
  static bool sTableChecked = false;
  if (!sTableChecked) {
    sTableChecked = true;
    for (const SectionExtras& x : kSectionExtras)
      if (!SettingsFields::sectionExists(x.section))
        log_w("gui: the section table names \"%s\", which the key table does not know",
              x.section);
  }

  size_t nSections = 0;
  for (size_t i = 0; i < SettingsFields::count(); i++) {
    const char* key = SettingsFields::keyAt(i);
    const char* dot = key ? strchr(key, '.') : nullptr;
    if (!dot) continue;
    const size_t n = (size_t)(dot - key);
    bool seen = false;
    for (size_t s = 0; s < nSections; s++)
      if (strncmp(sSections[s], key, n) == 0 && sSections[s][n] == 0) { seen = true; break; }
    if (seen || nSections >= kMaxSections || n >= sizeof(sSections[0])) continue;
    snprintf(sSections[nSections], sizeof(sSections[0]), "%.*s", (int)n, key);
    char label[20];
    snprintf(label, sizeof(label), "%s", sSections[nSections]);
    if (label[0] >= 'a' && label[0] <= 'z') label[0] -= 32;
    lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_RIGHT, label);
    lv_obj_add_event_cb(btn, openCategory, LV_EVENT_CLICKED, sSections[nSections]);
    nSections++;
  }

  lv_obj_t* ident = lv_list_add_button(list, LV_SYMBOL_HOME, "Identity");
  lv_obj_add_event_cb(ident, [](lv_event_t*) { Ui::openIdentity(); }, LV_EVENT_CLICKED, nullptr);

  // About lives here now — the action bar's slots belong to the spec's
  // three destinations plus the receiver.
  lv_obj_t* about = lv_list_add_button(list, LV_SYMBOL_LIST, "About");
  lv_obj_add_event_cb(about, [](lv_event_t*) { Ui::openAbout(); }, LV_EVENT_CLICKED, nullptr);

  push(Ui::screenOf(body));
}

} // namespace Ui

#endif // HAS_LVGL_UI
