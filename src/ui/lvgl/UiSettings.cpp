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
#include "WifiManager.h"

namespace {

// --- what kind of control a key wants ---------------------------------------

enum class Kind : uint8_t { Text, Secret, Number, Switch, Region, Words };

Kind kindFor(const char* key, const char* value, bool quoted) {
  if (strcmp(key, "radio.region") == 0)             return Kind::Region;
  if (strcmp(key, "wifi.security") == 0)            return Kind::Words;
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
    const SettingsFields::Result res = SettingsFields::set(r.key, value, err, sizeof(err));
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
lv_obj_t* sRadioFoot = nullptr;

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
  if (const Row* r = rowFor("radio.sf"))     { rowValue(*r, v, sizeof(v)); p.sf = (uint8_t)atoi(v); }
  if (const Row* r = rowFor("radio.bw_khz")) { rowValue(*r, v, sizeof(v)); p.bwKhz = (float)atof(v); }
  if (const Row* r = rowFor("radio.cr"))     { rowValue(*r, v, sizeof(v)); p.cr = (uint8_t)atoi(v); }
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

// The station's line on the wifi page, kept live while the page is open.
lv_obj_t* sStaStatus = nullptr;

void staStatusTick(lv_timer_t*) {
  if (!sStaStatus || !lv_obj_is_valid(sStaStatus)) return;
  const char* ssid = settings.wifi().staSsid;
  char line[96];
  if (!ssid[0]) {
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

  // The wifi page leads with the station: what it is doing right now, the
  // scanner to change it, and — while a network is saved — the way out. The
  // raw sta_ssid/sta_password rows are gone from the glass (the scanner and
  // its hidden-network dialog own joining; the console and portal still
  // carry the keys).
  if (strcmp(section, "wifi") == 0) {
    sStaStatus = lv_label_create(body);
    lv_obj_set_width(sStaStatus, lv_pct(100));
    lv_label_set_long_mode(sStaStatus, LV_LABEL_LONG_WRAP);
    lv_timer_t* t = lv_timer_create(staStatusTick, 1000, nullptr);
    lv_obj_add_event_cb(lv_obj_get_parent(lv_obj_get_parent(body)),
                        [](lv_event_t* ev) {
                          lv_timer_delete((lv_timer_t*)lv_event_get_user_data(ev));
                          sStaStatus = nullptr;
                        }, LV_EVENT_DELETE, t);
    staStatusTick(nullptr);

    lv_obj_t* scan = lv_button_create(body);
    lv_obj_set_width(scan, lv_pct(100));
    lv_obj_t* sl = lv_label_create(scan);
    lv_label_set_text(sl, LV_SYMBOL_WIFI "  Join a network...");
    lv_obj_center(sl);
    lv_obj_add_event_cb(scan, [](lv_event_t*) { Ui::openWifiJoin(); }, LV_EVENT_CLICKED, nullptr);

    if (wifiManager.stationConfigured()) {
      lv_obj_t* forget = lv_button_create(body);
      lv_obj_set_width(forget, lv_pct(100));
      lv_obj_t* fl = lv_label_create(forget);
      lv_label_set_text(fl, LV_SYMBOL_CLOSE "  Disconnect & forget");
      lv_obj_center(fl);
      lv_obj_add_event_cb(forget, [](lv_event_t* ev) {
        wifiManager.staForget();
        Ui::toast("Network forgotten");
        lv_obj_add_state((lv_obj_t*)lv_event_get_target(ev), LV_STATE_DISABLED);
      }, LV_EVENT_CLICKED, nullptr);
    }
  }

  memset(sRows, 0, sizeof(sRows));
  size_t used = 0;
  for (size_t i = 0; i < SettingsFields::count() && used < kMaxRows; i++) {
    if (!SettingsFields::keyInSection(i, section)) continue;
    // Joining lives on the scanner now — a picked network proves itself on
    // air before it is saved, and the hidden-SSID dialog covers the rest. A
    // raw row here would be a second, unverified way to say the same thing.
    const char* k = SettingsFields::keyAt(i);
    if (k && (strcmp(k, "wifi.sta_ssid") == 0 || strcmp(k, "wifi.sta_password") == 0)) continue;
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
  }

  if (strcmp(section, "radio") == 0) {
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
    lv_obj_add_event_cb(lv_obj_get_parent(lv_obj_get_parent(body)),
                        [](lv_event_t* ev) {
                          lv_timer_delete((lv_timer_t*)lv_event_get_user_data(ev));
                          sRadioFoot = nullptr;
                        }, LV_EVENT_DELETE, t);
    radioFootTick(nullptr);
  }

  if (strcmp(section, "display") == 0) {
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

  if (strcmp(section, "maintenance") == 0) {
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
    lv_obj_add_event_cb(erase, [](lv_event_t* e) {
      static uint8_t held = 0;
      const lv_event_code_t code = lv_event_get_code(e);
      if (code == LV_EVENT_PRESSED) held = 0;
      else if (code == LV_EVENT_LONG_PRESSED_REPEAT && ++held == 16) {
        settings.factoryReset();
        Ui::toast("erased — restarting");
        Bootloader::reboot(Bootloader::Source::Ui);
      }
    }, LV_EVENT_ALL, nullptr);
  }

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

  Ui::push(lv_obj_get_parent(lv_obj_get_parent(body)));
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

  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace Ui

#endif // HAS_LVGL_UI
