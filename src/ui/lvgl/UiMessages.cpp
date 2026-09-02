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
//  UiMessages.cpp — the inbox, a message, and the reply under the keys
//
//  Reading goes through Inbox::readPage exactly as the portal and the console
//  read; sending goes through the transport's reply queue, so the GUI hands
//  text to the Reticulum task and never touches the library.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "LxmfInbox.h"
#include "RnsTransport.h"

namespace {

lv_obj_t* sList = nullptr;
uint8_t   sReplyTo[16];
lv_obj_t* sReplyTa = nullptr;

void sendReply(lv_event_t*) {
  const char* text = lv_textarea_get_text(sReplyTa);
  if (!text || !*text) { Ui::toast("nothing to send"); return; }
  Ui::toast(RnsTransport::queueLxmfReply(sReplyTo, text)
            ? "queued for sending" : "could not queue — transport down?");
  lv_textarea_set_text(sReplyTa, "");
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
  sReplyTa = Ui::textarea(body, "reply…", false, false);
  lv_obj_set_height(sReplyTa, 52);
  lv_obj_t* send = lv_msgbox_add_footer_button(mbox, "Send");
  lv_obj_add_event_cb(send, sendReply, LV_EVENT_CLICKED, nullptr);
}

void rowClicked(lv_event_t* e) {
  const uint32_t seq = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  Rns::InboxRecord r;
  if (Rns::Inbox::read(seq, r)) openMessage(r);
}

void addRow(const Rns::InboxRecord& r, void*) {
  char line[64];
  snprintf(line, sizeof(line), "%02x%02x%02x%02x  %.*s", r.from[0], r.from[1],
           r.from[2], r.from[3], (int)(r.textLen > 34 ? 34 : r.textLen), r.text);
  lv_obj_t* btn = lv_list_add_button(sList, LV_SYMBOL_ENVELOPE, line);
  lv_obj_add_event_cb(btn, rowClicked, LV_EVENT_CLICKED, (void*)(uintptr_t)r.seq);
}

void refresh(lv_event_t* = nullptr) {
  lv_obj_clean(sList);
  const Rns::Inbox::Page pg = Rns::Inbox::readPage(0, 12, addRow, nullptr);
  if (!pg.count) lv_list_add_text(sList, "no messages yet");
}

} // namespace

namespace Ui {

void openMessages() {
  lv_obj_t* body = newScreen("Messages");

  lv_obj_t* bar = lv_button_create(body);
  lv_obj_t* lbl = lv_label_create(bar);
  lv_label_set_text(lbl, LV_SYMBOL_REFRESH " refresh");
  lv_obj_add_event_cb(bar, [](lv_event_t* e) { refresh(e); }, LV_EVENT_CLICKED, nullptr);

  sList = lv_list_create(body);
  lv_obj_set_width(sList, lv_pct(100));
  lv_obj_set_flex_grow(sList, 1);
  refresh();

  push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

} // namespace Ui

#endif // HAS_LVGL_UI
