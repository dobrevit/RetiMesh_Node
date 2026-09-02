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
//  UiHome.cpp — the home screen: readings above, shortcuts below
//
//  The phone convention, because the operator carries a phone: the status bar
//  says how the node is, the cards say what it is doing, and the row of icon
//  buttons at the bottom is where the functions live. A shortcut added later
//  is one button here, not a navigation redesign.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "Settings.h"
#include "Power.h"
#include "Gps.h"
#include "WifiManager.h"
#include "RnsTransport.h"
#include "LxmfInbox.h"

namespace {

lv_obj_t* sRadioVal = nullptr;
lv_obj_t* sNetVal = nullptr;
lv_obj_t* sNodeVal = nullptr;

lv_obj_t* card(lv_obj_t* parent, const char* title, lv_obj_t** valueOut) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_width(c, lv_pct(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(c, 8, 0);
  lv_obj_t* t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
  lv_obj_t* v = lv_label_create(c);
  lv_obj_set_width(v, lv_pct(100));
  lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
  lv_obj_align_to(v, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
  lv_label_set_text(v, "…");
  *valueOut = v;
  return c;
}

void refreshHome(lv_timer_t*) {
  // Only while home is what the operator sees; sub-screens have their own.
  if (!sRadioVal || !Ui::atRoot()) return;
  char v[160];

  if (g_stats.radioOnline)
    snprintf(v, sizeof(v), "%s  %.3f MHz  SF%d  %d dBm\n%lu received  /  %lu sent",
             g_stats.radioModel, (double)settings.radio().freqMhz,
             settings.radio().sf, (int)settings.radio().txDbm,
             (unsigned long)g_stats.loraRxPackets, (unsigned long)g_stats.loraTxPackets);
  else
    snprintf(v, sizeof(v), "offline");
  lv_label_set_text(sRadioVal, v);

  const RnsTransport::LxmfState lx = RnsTransport::lxmf();
  snprintf(v, sizeof(v), "LXMF  %s\n%lu message%s stored",
           lx.address[0] ? lx.address : "—",
           (unsigned long)Rns::Inbox::stored(),
           Rns::Inbox::stored() == 1 ? "" : "s");
  lv_label_set_text(sNetVal, v);

  const uint32_t up = millis() / 1000;
  size_t n = snprintf(v, sizeof(v), "%s\nup %lud %luh %lum",
                      FW_VERSION, (unsigned long)(up / 86400),
                      (unsigned long)(up % 86400 / 3600), (unsigned long)(up % 3600 / 60));
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.valid)
    snprintf(v + n, sizeof(v) - n, "\n%.5f, %.5f (%u sats)",
             f.latitude, f.longitude, f.satellites);
#endif
  lv_label_set_text(sNodeVal, v);
}

void shortcut(lv_obj_t* bar, const char* symbol, const char* name,
              void (*open)(lv_event_t*)) {
  lv_obj_t* btn = lv_button_create(bar);
  lv_obj_set_flex_grow(btn, 1);
  lv_obj_set_height(btn, 46);
  lv_obj_t* col = lv_label_create(btn);
  lv_label_set_text_fmt(col, "%s\n%s", symbol, name);
  lv_obj_set_style_text_align(col, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(col);
  lv_obj_add_event_cb(btn, open, LV_EVENT_CLICKED, nullptr);
}

} // namespace

namespace Ui {

void openHome() {
  lv_obj_t* body = newScreen(nullptr);   // home carries no back arrow
  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));

  card(body, "Radio", &sRadioVal);
  card(body, "Network", &sNetVal);
  card(body, "Node", &sNodeVal);

  // The shortcut bar, pinned to the bottom of the screen itself so the cards
  // scroll behind it rather than pushing it away.
  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), 50);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(bar, 2, 0);
  lv_obj_set_style_pad_column(bar, 2, 0);
  shortcut(bar, LV_SYMBOL_ENVELOPE, "Messages", [](lv_event_t*) { openMessages(); });
  shortcut(bar, LV_SYMBOL_SETTINGS, "Settings", [](lv_event_t*) { openSettings(); });
#if HAS_GPS
  shortcut(bar, LV_SYMBOL_GPS,      "GNSS",     [](lv_event_t*) { openGps(); });
#endif
  shortcut(bar, LV_SYMBOL_LIST,     "About",    [](lv_event_t*) { openAbout(); });

  // The bar steals the bottom 50 px from the card column.
  lv_obj_set_style_pad_bottom(body, 54, 0);

  lv_timer_create(refreshHome, 1000, nullptr);
  refreshHome(nullptr);

  lv_screen_load(scr);
}

void openAbout() {
  lv_obj_t* mbox = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(mbox, "RetiMesh Node");
  lv_msgbox_add_close_button(mbox);
  lv_obj_t* body = lv_msgbox_get_content(mbox);
  lv_obj_t* text = lv_label_create(body);
  const RnsTransport::LxmfState lx = RnsTransport::lxmf();
  char utc[24] = "";
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet) snprintf(utc, sizeof(utc), "\n%s UTC", f.utc);
#endif
  lv_label_set_text_fmt(text, "%s\n%s\n\nLXMF\n%s%s",
                        FW_VERSION, BOARD_NAME,
                        lx.address[0] ? lx.address : "—", utc);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(text, lv_pct(100));
}

} // namespace Ui

#endif // HAS_LVGL_UI
