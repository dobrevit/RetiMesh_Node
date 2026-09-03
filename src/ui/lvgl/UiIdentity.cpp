// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiIdentity.cpp — who this node is
//
//  The hash is grouped in fours across two lines because its one real-world
//  job is being verified out loud against another device. The QR carries
//  the same address for the phone that would rather scan than listen.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"
#include "WifiManager.h"
#include "RnsTransport.h"
#include "SettingsFields.h"
#include "QrCode.h"
#include "RnsAnnounce.h"
#include "LxmfInbox.h"
#include "PeerNames.h"
#include "Bootloader.h"

namespace {

void showQr(lv_event_t*) {
  char text[160];
  if (!Qr::payloadText(Qr::Payload::Address, text, sizeof(text))) {
    Ui::toast("no address yet");
    return;
  }
  QRCode qr;
  uint8_t buf[Qr::MAX_BUFFER];
  if (!Qr::encode(text, qr, buf)) { Ui::toast("could not encode"); return; }

  const int scale = 3, quiet = 4;
  const int px = (qr.size + quiet * 2) * scale;
  static uint8_t* cbuf = nullptr;        // one code on screen at a time
  cbuf = (uint8_t*)lv_realloc(cbuf, (size_t)px * px * 2);
  if (!cbuf) { Ui::toast("no memory for the code"); return; }

  lv_obj_t* box = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(box, "LXMF address");
  lv_msgbox_add_close_button(box);
  lv_obj_t* body = lv_msgbox_get_content(box);
  lv_obj_t* canvas = lv_canvas_create(body);
  lv_canvas_set_buffer(canvas, cbuf, px, px, LV_COLOR_FORMAT_RGB565);
  lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++)
            lv_canvas_set_px(canvas, (x + quiet) * scale + dx, (y + quiet) * scale + dy,
                             lv_color_black(), LV_OPA_COVER);
}

} // namespace

namespace Ui {

void openIdentity() {
  lv_obj_t* body = newScreen("Identity");

  UiTheme::reading(body, "NAME", wifiManager.hostname());

  const RnsTransport::LxmfState lx = RnsTransport::lxmf();
  lv_obj_t* hashCard = lv_obj_create(body);
  UiTheme::card(hashCard);
  lv_obj_set_width(hashCard, lv_pct(100));
  lv_obj_set_height(hashCard, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(hashCard, 8, 0);
  lv_obj_t* hl = lv_label_create(hashCard);
  lv_label_set_text(hl, "LXMF ADDRESS");
  UiTheme::labelCaps(hl);
  lv_obj_t* hash = lv_label_create(hashCard);
  char grouped[48];
  Ui::groupedHash(lx.address[0] ? lx.address : "not up yet", grouped, sizeof(grouped));
  lv_label_set_text(hash, grouped);
  lv_obj_set_style_text_font(hash, &font_plexmono_16, 0);
  lv_obj_align_to(hash, hl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

  char v[32];
  if (SettingsFields::renderKey("radio.announce_interval", v, sizeof(v))) {
    const char* eq = strchr(v, '=');
    UiTheme::reading(body, "ANNOUNCE EVERY", eq ? eq + 1 : v);
  }

  lv_obj_t* row = lv_obj_create(body);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 40);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 6, 0);
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
  lv_obj_t* qr = lv_button_create(row);
  UiTheme::actionButton(qr);
  lv_obj_set_flex_grow(qr, 1);
  lv_obj_set_height(qr, lv_pct(100));
  lv_obj_t* ql = lv_label_create(qr);
  lv_label_set_text(ql, "QR");
  lv_obj_center(ql);
  lv_obj_add_event_cb(qr, showQr, LV_EVENT_CLICKED, nullptr);

  // The way to the key's death — a door, not a trigger: the destruction
  // itself lives behind its own screen's words and hold.
  lv_obj_t* erase = lv_button_create(body);
  UiTheme::actionButton(erase);
  lv_obj_set_width(erase, lv_pct(100));
  lv_obj_set_height(erase, 40);
  lv_obj_t* el = lv_label_create(erase);
  lv_label_set_text(el, "ERASE THIS IDENTITY " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(el, lv_color_hex(UiTheme::kBad), 0);
  lv_obj_center(el);
  lv_obj_add_event_cb(erase, [](lv_event_t*) { openEraseIdentity(); },
                      LV_EVENT_CLICKED, nullptr);

  push(Ui::screenOf(body));
}

// Design screen 21: hold-to-confirm with its own words, never a second
// dialog — resistive glass and gloves produce stray taps, never stray holds.
void openEraseIdentity() {
  lv_obj_t* body = newScreen("Erase identity");

  lv_obj_t* card = lv_obj_create(body);
  UiTheme::card(card);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_t* cl = lv_label_create(card);
  lv_label_set_text(cl, "THIS IDENTITY");
  UiTheme::labelCaps(cl);
  lv_obj_t* hash = lv_label_create(card);
  char grouped[48];
  Ui::groupedHash(nodeIdentity.identityHex(), grouped, sizeof(grouped));
  lv_label_set_text(hash, grouped);
  lv_obj_set_style_text_font(hash, &font_plexmono_16, 0);
  lv_obj_align_to(hash, cl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

  lv_obj_t* copy = lv_label_create(body);
  lv_label_set_text(copy,
      "Destroys this node's Reticulum keys. The next start invents a new "
      "identity \u2014 to every peer, a stranger. Conversations and "
      "remembered names go with the keys. Radio and network settings stay. "
      "There is no undo and no export after this point.");
  lv_obj_set_width(copy, lv_pct(100));
  lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);

  lv_obj_t* fire = lv_button_create(body);
  UiTheme::actionButton(fire);
  lv_obj_set_width(fire, lv_pct(100));
  lv_obj_set_height(fire, 44);
  lv_obj_t* fl = lv_label_create(fire);
  lv_label_set_text(fl, "ERASE IDENTITY \u2014 HOLD 2 s");
  lv_obj_set_style_text_color(fl, lv_color_hex(UiTheme::kBad), 0);
  lv_obj_center(fl);
  Ui::onHeld2s(fire, [](void*) {
    nodeIdentity.destroy();
    Rns::Inbox::wipe();
    PeerNames::wipe();
    Ui::toast("identity erased \u2014 restarting");
    Bootloader::reboot(Bootloader::Source::Ui);
  }, nullptr);

  push(Ui::screenOf(body));
}

} // namespace Ui

#endif // HAS_LVGL_UI
