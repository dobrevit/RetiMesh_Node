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
#include "UiTheme.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include <esp_timer.h>
#include "Settings.h"
#include "Power.h"
#include "Gps.h"
#include "WifiManager.h"
#include "RnsTransport.h"
#include "LxmfInbox.h"

namespace {

lv_obj_t* sClock = nullptr;
lv_obj_t* sDate = nullptr;
lv_obj_t* sGnssVal = nullptr;
lv_obj_t* sPeersVal = nullptr;
lv_obj_t* sRssiVal = nullptr;
lv_obj_t* sPosVal = nullptr;
lv_obj_t* sBattVal = nullptr;
lv_obj_t* sRadioVal = nullptr;
lv_obj_t* sLatestVal = nullptr;

lv_obj_t* reading(lv_obj_t* parent, const char* label) {
  return UiTheme::reading(parent, label, nullptr);
}

void tintIf(lv_obj_t* l, uint32_t hex) {
  static struct { lv_obj_t* l; uint32_t c; } last[8];
  for (auto& e : last)
    if (e.l == l || e.l == nullptr) {
      if (e.l == l && e.c == hex) return;
      e.l = l; e.c = hex;
      break;
    }
  lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
}

void refreshHome(lv_timer_t*) {
  // Only while home is what the operator sees — sub-screens have their own,
  // and the idle clock is an opaque overlay this refresh once kept painting
  // pixels behind, every second, in the exact state the device rests in.
  if (!sClock || !Ui::atRoot() || Ui::idleShowing()) return;
  char v[160];

  // Clock and date straight off the receiver, as the spec draws them; the
  // trust flag beside them is the semantic story — green means the time is
  // GNSS-locked, amber means the receiver lost it, gray means never had it.
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet && strlen(f.utc) >= 16) {
    char hm[6] = { f.utc[11], f.utc[12], f.utc[13], f.utc[14], f.utc[15], 0 };
    Ui::setLabel(sClock, hm);
    char d[20];
    snprintf(d, sizeof(d), "%.10s UTC", f.utc);
    Ui::setLabel(sDate, d);
  } else {
    Ui::setLabel(sClock, "--:--");
    Ui::setLabel(sDate, "time not set");
  }
  if (f.valid && f.clockSet)      { Ui::setLabel(sGnssVal, "locked");  tintIf(sGnssVal, UiTheme::kGood); }
  else if (f.clockSet)            { Ui::setLabel(sGnssVal, "drift");   tintIf(sGnssVal, UiTheme::kWarn); }
  else                            { Ui::setLabel(sGnssVal, "unset");   tintIf(sGnssVal, UiTheme::kInkLabel); }
  if (f.valid) snprintf(v, sizeof(v), "%.5f %.5f", f.latitude, f.longitude);
  else snprintf(v, sizeof(v), "no fix · %u sv", f.satellites);
  Ui::setLabel(sPosVal, v);
  tintIf(sPosVal, f.valid ? UiTheme::kInk : UiTheme::kInkLabel);
#else
  Ui::setLabel(sClock, "--:--");
  Ui::setLabel(sDate, "no receiver");
#endif

  snprintf(v, sizeof(v), "%lu", (unsigned long)RnsTransport::pathCount());
  Ui::setLabel(sPeersVal, v);

  if (g_stats.loraRxPackets) {
    snprintf(v, sizeof(v), "%.0f dBm", (double)g_stats.lastRssi);
    Ui::setLabel(sRssiVal, v);
    tintIf(sRssiVal, g_stats.lastRssi > -100 ? UiTheme::kInk : UiTheme::kWarn);
  } else Ui::setLabel(sRssiVal, "—");

  const Power::Battery b = Power::battery();
  if (b.present) {
    snprintf(v, sizeof(v), "%u%% · %.2f V%s", b.percent, (double)b.volts,
             (b.chargeKnown && b.charging) ? " · chg" : "");
    Ui::setLabel(sBattVal, v);
    tintIf(sBattVal, b.percent >= 25 ? UiTheme::kInk
                                     : (b.percent >= 10 ? UiTheme::kWarn : UiTheme::kBad));
  } else Ui::setLabel(sBattVal, "external");

  if (g_stats.radioOnline)
    // The rx/tx pair is the one-glance "is this node hearing anything" the
    // field checks live on; it left with the old cards and is missed.
    snprintf(v, sizeof(v), "%.3f SF%d · %lu rx %lu tx", (double)settings.radio().freqMhz,
             settings.radio().sf, (unsigned long)g_stats.loraRxPackets,
             (unsigned long)g_stats.loraTxPackets);
  else
    snprintf(v, sizeof(v), "offline");
  Ui::setLabel(sRadioVal, v);
  tintIf(sRadioVal, g_stats.radioOnline ? UiTheme::kInk : UiTheme::kWarn);

  // The newest message, one line — and only when there is news: the read
  // behind this opens flash, and doing that every second forever on the
  // resting screen was ~86k needless reads a day.
  static uint32_t sLastSeq = UINT32_MAX;
  const uint32_t seq = Rns::Inbox::newest();
  if (seq == sLastSeq) return;
  sLastSeq = seq;
  struct Latest { char line[80]; bool got; } latest{{0}, false};
  Rns::Inbox::readPage(0, 1, [](const Rns::InboxRecord& r, void* ctx) {
    Latest* o = (Latest*)ctx;
    char who[34];
    Ui::peerLabel(r.from, who, sizeof(who));
    const size_t n = r.textLen < 40 ? r.textLen : 40;
    snprintf(o->line, sizeof(o->line), "%s · %.*s", who, (int)n, r.text);
    o->got = true;
  }, &latest);
  Ui::setLabel(sLatestVal, latest.got ? latest.line : "no messages yet");
}

void shortcut(lv_obj_t* bar, const char* symbol, const char* name,
              void (*open)(lv_event_t*)) {
  lv_obj_t* btn = lv_button_create(bar);
  UiTheme::actionButton(btn);
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

  // The board, in the spec's order: the clock first, then the readings.
  sClock = lv_label_create(body);
  lv_obj_set_style_text_font(sClock, &font_barlow_28, 0);
  lv_label_set_text(sClock, "--:--");
  sDate = lv_label_create(body);
  lv_obj_set_style_text_color(sDate, lv_color_hex(UiTheme::kInkDim), 0);
  lv_label_set_text(sDate, "");
  sGnssVal  = reading(body, "GNSS TIME");
  sPeersVal = reading(body, "PEERS SEEN");
  sRssiVal  = reading(body, "LAST RSSI");
  sPosVal   = reading(body, "POSITION");
  sBattVal  = reading(body, "BATTERY");
  sRadioVal = reading(body, "RADIO");
  lv_obj_t* latestRow = lv_obj_create(body);
  UiTheme::card(latestRow);
  lv_obj_set_width(latestRow, lv_pct(100));
  lv_obj_set_height(latestRow, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(latestRow, 8, 0);
  lv_obj_t* ll = lv_label_create(latestRow);
  lv_label_set_text(ll, "LATEST");
  UiTheme::labelCaps(ll);
  sLatestVal = lv_label_create(latestRow);
  UiTheme::value(sLatestVal);
  lv_obj_set_width(sLatestVal, lv_pct(100));
  lv_label_set_long_mode(sLatestVal, LV_LABEL_LONG_DOT);
  lv_obj_align_to(sLatestVal, ll, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);
  lv_label_set_text(sLatestVal, "…");

  // The shortcut bar, pinned to the bottom of the screen itself so the cards
  // scroll behind it rather than pushing it away.
  lv_obj_t* bar = lv_obj_create(scr);
  lv_obj_remove_style_all(bar);
  UiTheme::bar(bar);
  lv_obj_set_size(bar, lv_pct(100), 50);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_all(bar, 2, 0);
  lv_obj_set_style_pad_column(bar, 2, 0);
  // The action bar, in the design's vocabulary: terse caps, always the same
  // slots. MESH joins the row when the destinations screen exists.
  shortcut(bar, LV_SYMBOL_ENVELOPE, "MSG",  [](lv_event_t*) { openMessages(); });
  shortcut(bar, LV_SYMBOL_SHUFFLE,  "MESH", [](lv_event_t*) { openDestinations(); });
#if HAS_GPS
  shortcut(bar, LV_SYMBOL_GPS,      "GPS",  [](lv_event_t*) { openGps(); });
#endif
  shortcut(bar, LV_SYMBOL_SETTINGS, "SET",  [](lv_event_t*) { openSettings(); });

  // The bar steals the bottom 50 px from the card column.
  lv_obj_set_style_pad_bottom(body, 54, 0);

  lv_timer_create(refreshHome, 1000, nullptr);
  refreshHome(nullptr);

  lv_obj_t* prev = lv_screen_active();   // the boot splash, its job done
  lv_screen_load(scr);
  if (prev && prev != scr) lv_obj_delete(prev);
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
