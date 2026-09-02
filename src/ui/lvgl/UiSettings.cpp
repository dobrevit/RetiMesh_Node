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

#if HAS_LVGL_UI

#include <Arduino.h>
#include "SettingsFields.h"
#include "Settings.h"
#include "Power.h"
#include "Airtime.h"

namespace {

// --- what kind of control a key wants ---------------------------------------

enum class Kind : uint8_t { Text, Secret, Number, Switch, Region, Words };

struct WordList { const char* options; };  // '\n'-separated, LVGL dropdown form

// The canonical word sets, from the same helpers the funnel validates with.
constexpr const char* kSecurityWords = "open\nwpa2\nwpa2wpa3\nwpa3";
constexpr const char* kProfileWords  = "performance\nbalanced\nbattery";

Kind kindFor(const char* key, const char* value) {
  if (strcmp(key, "radio.region") == 0)             return Kind::Region;
  if (strcmp(key, "wifi.security") == 0)            return Kind::Words;
  if (strcmp(key, "transport.power_profile") == 0)  return Kind::Words;
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

const char* wordsFor(const char* key) {
  return strcmp(key, "wifi.security") == 0 ? kSecurityWords : kProfileWords;
}

// --- one form row ------------------------------------------------------------

// A row remembers what it started as, so Save can skip what did not change
// and Cancel can simply leave.
struct Row {
  char      key[40];
  char      initial[96];
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
  char now[96];
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
      const char* words = wordsFor(r.key);
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
  for (Row& r : sRows) {
    if (!r.used || !rowChanged(r)) continue;
    char value[96], err[128] = "";
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
  char line[160];
  if (failed) snprintf(line, sizeof(line), "%u saved, %u refused — %s",
                       (unsigned)changed, (unsigned)failed, firstErr);
  else if (changed) snprintf(line, sizeof(line), "%u setting%s saved",
                             (unsigned)changed, changed == 1 ? "" : "s");
  else snprintf(line, sizeof(line), "nothing changed");
  Ui::toast(line);
  if (!failed) Ui::back();               // a refusal keeps the form for fixing
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

  memset(sRows, 0, sizeof(sRows));
  size_t used = 0;
  for (size_t i = 0; i < SettingsFields::count() && used < kMaxRows; i++) {
    if (!SettingsFields::keyInSection(i, section)) continue;
    char line[224];
    if (!SettingsFields::render(i, line, sizeof(line))) continue;
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char* value = eq + 1;
    // Strings render quoted; the editor wants the bare value.
    size_t vn = strlen(value);
    if (vn >= 2 && value[0] == '"' && value[vn - 1] == '"') { value[vn - 1] = 0; value++; }

    Row& r = sRows[used++];
    r.used = true;
    strlcpy(r.key, line, sizeof(r.key));
    strlcpy(r.initial, value, sizeof(r.initial));
    r.kind = kindFor(r.key, value);

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

  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace Ui

#endif // HAS_LVGL_UI
