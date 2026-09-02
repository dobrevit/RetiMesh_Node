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
//  LvglUi.cpp — see LvglUi.h
//
//  Structure: one tabview, three tabs, and a keyboard that exists once and
//  attaches to whichever textarea has focus. The Settings tab is deliberately
//  generic — it renders the console's own key table and edits any key through
//  the console's own funnel, so every setting the node has is editable on the
//  glass today, and a setting added next month appears here without this file
//  changing. The polish of purpose-built forms can come screen by screen; the
//  capability comes now.
// ============================================================================
#include "LvglUi.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include <lvgl.h>
#include "TftPanel.h"
#include "TouchInput.h"
#include "LxmfInbox.h"
#include "RnsTransport.h"
#include "SettingsFields.h"
#include "Settings.h"
#include "Power.h"
#include "Gps.h"
#include "WifiManager.h"

namespace {

TftPanel* sPanel = nullptr;

// Two partial render buffers, ~1/10 of the panel each, in internal DRAM: small
// enough to stay DMA-friendly, and the PSRAM-buffer-over-SPI chunking trap
// (esp-bsp #726) never applies. LVGL renders into one while the other drains.
constexpr size_t kBufPx = DISPLAY_WIDTH * 32;
static uint8_t sBuf1[kBufPx * 2];
static uint8_t sBuf2[kBufPx * 2];

lv_display_t* sDisp = nullptr;
lv_obj_t* sTabs = nullptr;
lv_obj_t* sKeyboard = nullptr;           // one keyboard, re-aimed at the focused field

// --- plumbing ---------------------------------------------------------------

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  // LVGL renders little-endian RGB565; the ST7789 eats big-endian.
  const uint32_t n = (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px, n);
  sPanel->blitArea((int16_t)area->x1, (int16_t)area->y1,
                   (int16_t)area->x2, (int16_t)area->y2, px);
  lv_display_flush_ready(disp);
}

void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  const TouchInput::Point p = TouchInput::poll();
  data->state = p.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (p.down) { data->point.x = p.x; data->point.y = p.y; }
}

// The keyboard: created once, shown under whichever textarea gains focus,
// hidden when the field is done. LV_KEYBOARD_MODE_TEXT_LOWER starts it as the
// familiar mobile layout; the widget's own keys switch case/symbols/numbers.
void kbEvent(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

void taFocusEvent(lv_event_t* e) {
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_keyboard_set_textarea(sKeyboard, ta);
    lv_obj_remove_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
    // The editors are modal message boxes, and modals live on the same top
    // layer as the keyboard — created later, they stack above it, which on
    // the bench put the keys underneath the dialog and out of reach. The
    // keyboard comes to the front of the layer every time it is summoned,
    // and the dialog is lifted to the top of the glass so the field sits in
    // the upper half while the keys take the lower.
    lv_obj_move_foreground(sKeyboard);
    // The object to move is the dialog, not its backdrop: a modal msgbox sits
    // inside a full-screen modal container on the top layer, and aligning
    // that container moves nothing anyone can see — measured on the bench as
    // a keyboard still fighting the dialog for the same pixels. Walk up to
    // the container, keep the child we arrived through (the dialog), pin it
    // to the top and cap its height to exactly the glass above the keys; its
    // content scrolls if the message is long.
    lv_obj_t* dlg = ta;
    lv_obj_t* up  = lv_obj_get_parent(dlg);
    while (up && up != lv_layer_top() && up != lv_screen_active()) {
      dlg = up;
      up = lv_obj_get_parent(dlg);
    }
    if (up == lv_layer_top() && dlg != sKeyboard) {
      lv_obj_t* box = dlg;
      // dlg is the backdrop when the dialog is modal; the dialog is the
      // backdrop's child on our path. Find it by walking one level down
      // toward the textarea.
      for (lv_obj_t* n = ta; n && n != dlg; n = lv_obj_get_parent(n))
        if (lv_obj_get_parent(n) == dlg) { box = n; break; }
      const int32_t kbTop = LV_VER_RES - lv_obj_get_height(sKeyboard);
      lv_obj_set_style_max_height(box, kbTop - 10, 0);
      lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 4);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
  } else if (code == LV_EVENT_DEFOCUSED) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t* makeTextarea(lv_obj_t* parent, const char* placeholder, bool oneLine) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, oneLine);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_FOCUSED, nullptr);
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_DEFOCUSED, nullptr);
  return ta;
}

// A transient line at the top of the screen: the result of a save or a send.
void toast(const char* text) {
  lv_obj_t* t = lv_label_create(lv_layer_top());
  lv_label_set_text(t, text);
  lv_obj_set_style_bg_opa(t, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(t, lv_color_black(), 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_pad_all(t, 6, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
  lv_obj_delete_delayed(t, 2500);
}

// --- Status tab -------------------------------------------------------------

lv_obj_t* sStatusTable = nullptr;

void refreshStatus(lv_timer_t*) {
  if (!sStatusTable || lv_tabview_get_tab_active(sTabs) != 0) return;
  char v[48];
  size_t row = 0;
  auto put = [&](const char* k, const char* val) {
    lv_table_set_cell_value(sStatusTable, row, 0, k);
    lv_table_set_cell_value(sStatusTable, row, 1, val);
    row++;
  };
  put("Node", wifiManager.ssid());
  put("Version", FW_VERSION);
  const uint32_t up = millis() / 1000;
  snprintf(v, sizeof(v), "%lud %luh %lum", (unsigned long)(up / 86400),
           (unsigned long)(up % 86400 / 3600), (unsigned long)(up % 3600 / 60));
  put("Uptime", v);
  if (g_stats.radioOnline) {
    snprintf(v, sizeof(v), "%s %.3f MHz SF%d", g_stats.radioModel,
             (double)settings.radio().freqMhz, settings.radio().sf);
  } else {
    snprintf(v, sizeof(v), "offline");
  }
  put("Radio", v);
  snprintf(v, sizeof(v), "%lu rx / %lu tx", (unsigned long)g_stats.loraRxPackets,
           (unsigned long)g_stats.loraTxPackets);
  put("Packets", v);
  const Power::Battery b = Power::battery();
  if (b.present) snprintf(v, sizeof(v), "%u%% (%.2f V)%s", b.percent, (double)b.volts,
                          b.chargeKnown ? (b.charging ? " charging" : "") : "");
  else           snprintf(v, sizeof(v), "none seen");
  put("Battery", v);
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.valid) snprintf(v, sizeof(v), "%.5f %.5f (%u sats)", f.latitude, f.longitude, f.satellites);
  else         snprintf(v, sizeof(v), f.enabled ? "no fix yet" : "off");
  put("GNSS", v);
#endif
  const RnsTransport::LxmfState lx = RnsTransport::lxmf();
  if (lx.address[0]) put("LXMF", lx.address);
  put("Msgs stored", (snprintf(v, sizeof(v), "%lu", (unsigned long)Rns::Inbox::stored()), v));
}

void buildStatusTab(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 4, 0);
  sStatusTable = lv_table_create(tab);
  lv_obj_set_width(sStatusTable, lv_pct(100));
  lv_table_set_column_width(sStatusTable, 0, 78);
  lv_table_set_column_width(sStatusTable, 1, 148);
  lv_timer_create(refreshStatus, 1000, nullptr);
  refreshStatus(nullptr);
}

// --- Messages tab -----------------------------------------------------------

lv_obj_t* sMsgList = nullptr;
uint8_t   sReplyTo[16];
lv_obj_t* sReplyTa = nullptr;

void sendReply(lv_event_t*) {
  const char* text = lv_textarea_get_text(sReplyTa);
  if (!text || !*text) { toast("nothing to send"); return; }
  // Handed to the Reticulum task; the GUI never touches the library. The
  // queue answer only says it was accepted for sending.
  toast(RnsTransport::queueLxmfReply(sReplyTo, text)
        ? "queued for sending" : "could not queue — transport down?");
  lv_textarea_set_text(sReplyTa, "");
  lv_keyboard_set_textarea(sKeyboard, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
}

void openMessage(const Rns::InboxRecord& r) {
  memcpy(sReplyTo, r.from, 16);
  lv_obj_t* mbox = lv_msgbox_create(nullptr);
  char head[40];
  snprintf(head, sizeof(head), "%02x%02x%02x%02x… (%s)", r.from[0], r.from[1],
           r.from[2], r.from[3], Rns::standingName(r.standing));
  lv_msgbox_add_title(mbox, head);
  lv_msgbox_add_close_button(mbox);
  lv_obj_t* body = lv_msgbox_get_content(mbox);
  lv_obj_t* text = lv_label_create(body);
  lv_label_set_text_fmt(text, "%.*s", (int)r.textLen, r.text);
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(text, lv_pct(100));
  sReplyTa = makeTextarea(body, "reply…", false);
  lv_obj_set_height(sReplyTa, 56);
  lv_obj_t* send = lv_msgbox_add_footer_button(mbox, "Send");
  lv_obj_add_event_cb(send, sendReply, LV_EVENT_CLICKED, nullptr);
}

struct MsgRowCtx { uint32_t seq; };

void msgRowClicked(lv_event_t* e) {
  const uint32_t seq = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  Rns::InboxRecord r;
  if (Rns::Inbox::read(seq, r)) openMessage(r);
}

void addMsgRow(const Rns::InboxRecord& r, void*) {
  char line[64];
  snprintf(line, sizeof(line), "%02x%02x%02x%02x  %.*s", r.from[0], r.from[1],
           r.from[2], r.from[3], (int)(r.textLen > 34 ? 34 : r.textLen), r.text);
  lv_obj_t* btn = lv_list_add_button(sMsgList, LV_SYMBOL_ENVELOPE, line);
  lv_obj_add_event_cb(btn, msgRowClicked, LV_EVENT_CLICKED, (void*)(uintptr_t)r.seq);
}

void refreshMessages(lv_event_t* = nullptr) {
  lv_obj_clean(sMsgList);
  const Rns::Inbox::Page pg = Rns::Inbox::readPage(0, 12, addMsgRow, nullptr);
  if (!pg.count) lv_list_add_text(sMsgList, "no messages yet");
}

void buildMessagesTab(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 2, 0);
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* bar = lv_button_create(tab);
  lv_obj_t* lbl = lv_label_create(bar);
  lv_label_set_text(lbl, LV_SYMBOL_REFRESH " refresh");
  lv_obj_add_event_cb(bar, [](lv_event_t* e) { refreshMessages(e); }, LV_EVENT_CLICKED, nullptr);
  sMsgList = lv_list_create(tab);
  lv_obj_set_width(sMsgList, lv_pct(100));
  lv_obj_set_flex_grow(sMsgList, 1);
  refreshMessages();
}

// --- Settings tab -----------------------------------------------------------
//
// Generic on purpose: the console's key table rendered as a list, one editor
// for any key, every save through SettingsFields::set — the same words, the
// same refusals, the same apply-live behaviour as SET at the console.

lv_obj_t* sSetList = nullptr;
char sEditKey[48];
lv_obj_t* sEditTa = nullptr;

void refreshSettingsList();

void saveSetting(lv_event_t*) {
  char detail[192] = "";
  const SettingsFields::Result res =
      SettingsFields::set(sEditKey, lv_textarea_get_text(sEditTa), detail, sizeof(detail));
  const bool ok = res == SettingsFields::Result::Ok ||
                  res == SettingsFields::Result::OkRestart ||
                  res == SettingsFields::Result::OkNextBoot;
  toast(detail[0] ? detail : SettingsFields::resultText(res));
  if (ok) refreshSettingsList();
}

void openEditor(lv_event_t* e) {
  const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
  const char* key = SettingsFields::keyAt(i);
  if (!key) return;
  strlcpy(sEditKey, key, sizeof(sEditKey));

  char line[224] = "";
  SettingsFields::render(i, line, sizeof(line));
  const char* eq = strchr(line, '=');

  lv_obj_t* mbox = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(mbox, key);
  lv_msgbox_add_close_button(mbox);
  sEditTa = makeTextarea(lv_msgbox_get_content(mbox), "value", true);
  // Secrets render as "(set)"; starting the editor from that would save the
  // placeholder as the password. Start those empty.
  if (eq && strcmp(eq + 1, "(set)") != 0 && strcmp(eq + 1, "(unset)") != 0)
    lv_textarea_set_text(sEditTa, eq + 1);
  lv_obj_t* save = lv_msgbox_add_footer_button(mbox, "Save");
  lv_obj_add_event_cb(save, saveSetting, LV_EVENT_CLICKED, nullptr);
}

void refreshSettingsList() {
  lv_obj_clean(sSetList);
  const char* section = "";
  for (size_t i = 0; i < SettingsFields::count(); i++) {
    char line[224];
    if (!SettingsFields::render(i, line, sizeof(line))) continue;
    // A section header whenever the prefix changes: radio.*, wifi.*, ...
    const char* key = SettingsFields::keyAt(i);
    const char* dot = strchr(key, '.');
    if (dot) {
      const size_t n = (size_t)(dot - key);
      if (strncmp(section, key, n) != 0 || section[n] != '\0') {
        static char sec[24];
        snprintf(sec, sizeof(sec), "%.*s", (int)n, key);
        section = sec;
        lv_list_add_text(sSetList, sec);
      }
    }
    lv_obj_t* btn = lv_list_add_button(sSetList, nullptr, line);
    lv_obj_add_event_cb(btn, openEditor, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
  }
}

void buildSettingsTab(lv_obj_t* tab) {
  lv_obj_set_style_pad_all(tab, 2, 0);
  sSetList = lv_list_create(tab);
  lv_obj_set_size(sSetList, lv_pct(100), lv_pct(100));
  refreshSettingsList();
}

} // namespace

// --- the module -------------------------------------------------------------

namespace LvglUi {

bool begin(TftPanel& panel) {
  sPanel = &panel;
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  sDisp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!sDisp) return false;
  lv_display_set_flush_cb(sDisp, flushCb);
  lv_display_set_buffers(sDisp, sBuf1, sBuf2, sizeof(sBuf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touchCb);

  // The shell: three tabs, and the keyboard that serves them all.
  sTabs = lv_tabview_create(lv_screen_active());
  lv_tabview_set_tab_bar_size(sTabs, 32);
  buildStatusTab(lv_tabview_add_tab(sTabs, "Status"));
  buildMessagesTab(lv_tabview_add_tab(sTabs, "Messages"));
  buildSettingsTab(lv_tabview_add_tab(sTabs, "Settings"));

  sKeyboard = lv_keyboard_create(lv_layer_top());
  // Four rows at ~33 px: comfortably tappable at this dot pitch, and 188 px
  // of glass stay free above it for whatever is being typed into.
  lv_obj_set_height(sKeyboard, 132);
  lv_keyboard_set_mode(sKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(sKeyboard, kbEvent, LV_EVENT_ALL, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);

  log_i("gui: LVGL %d.%d.%d up — 3 tabs, %u B x2 render buffers",
        lv_version_major(), lv_version_minor(), lv_version_patch(),
        (unsigned)sizeof(sBuf1));
  return true;
}

uint32_t loop() { return lv_timer_handler(); }

void stepTab(int8_t dir) {
  if (!sTabs) return;
  const uint32_t n = 3;
  uint32_t cur = lv_tabview_get_tab_active(sTabs);
  lv_tabview_set_active(sTabs, (cur + n + (dir > 0 ? 1 : n - 1)) % n, LV_ANIM_ON);
}

void onBlank(bool on) {
  // The panel driver darkens the glass; on the way back everything must be
  // repainted, because the controller's RAM kept the image but LVGL does not
  // know the backlight cycled.
  if (!on && sDisp) lv_obj_invalidate(lv_screen_active());
}

} // namespace LvglUi

#endif // HAS_LVGL_UI
