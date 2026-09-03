// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiMessages.cpp — conversations, the way the spec draws them
//
//  The list groups the inbox by sender; a thread interleaves what they said
//  with what this node answered, direction carried by alignment and one
//  accent edge — no avatars, no coloured bubbles. Our messages carry their
//  honest fate from the transport's outbound log: queued in amber, sent with
//  its age, failed in red. Reading goes through Inbox::readPage exactly as
//  the portal and the console read; sending through the transport's reply
//  queue — the GUI never touches the library.
//
//  Quick messages are the fast path for gloves: one tap, no keyboard. The
//  distress line is the only red control in the system and the only one that
//  requires a real two-second hold — a resistive panel under rain produces
//  stray taps, never a stray hold.
// ============================================================================
#include "Ui.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include "UiTheme.h"
#include "LxmfInbox.h"
#include "RnsTransport.h"
#include "Gps.h"
#include "Neighbors.h"
#include "PeerNames.h"

namespace {

lv_obj_t* sList = nullptr;               // the conversations screen's list
lv_obj_t* sThreadCol = nullptr;          // the open thread's bubble column
lv_obj_t* sThreadTa = nullptr;
uint8_t   sThreadDest[16];
uint32_t  sThreadStamp = 0;              // outbound-state hash, to redraw only on change



// --- the thread -------------------------------------------------------------

struct Bubble {
  bool     ours;
  uint32_t ms;                           // 0 = an older boot; sorts oldest
  char     text[81];
  uint32_t sentMs;                       // ours only
  bool     ok;
  uint8_t  standing;                     // theirs only (Rns::Standing*)
};

void bubbleRow(lv_obj_t* col, const Bubble& b) {
  lv_obj_t* row = lv_obj_create(col);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);

  lv_obj_t* box = lv_obj_create(row);
  UiTheme::card(box);
  lv_obj_set_width(box, lv_pct(82));
  lv_obj_set_height(box, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(box, 6, 0);
  // Direction is the alignment plus one accent edge, as the spec draws it.
  lv_obj_set_style_border_side(box, b.ours ? LV_BORDER_SIDE_RIGHT : LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(box, 2, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(b.ours ? UiTheme::kAccent : UiTheme::kEdge), 0);
  lv_obj_align(box, b.ours ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* t = lv_label_create(box);
  lv_label_set_text(t, b.text);
  lv_obj_set_width(t, lv_pct(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

  // The state rides the meta line, in the state's colour.
  char meta[32];
  uint32_t tint = UiTheme::kInkLabel;
  if (b.ours) {
    if (!b.sentMs)      { snprintf(meta, sizeof(meta), "queued"); tint = UiTheme::kWarn; }
    else if (!b.ok)     { snprintf(meta, sizeof(meta), "failed — no key?"); tint = UiTheme::kBad; }
    else { char a[8]; Ui::ageTextMs(millis() - b.sentMs, a, sizeof(a)); snprintf(meta, sizeof(meta), "sent · %s ago", a); }
  } else {
    // Identity trust first: an unverified sender's words carry the flag in
    // the meta line, in the warning colour.
    const bool verified = b.standing == Rns::StandingVerified;
    char a[8] = "";
    if (b.ms) Ui::ageTextMs(millis() - b.ms, a, sizeof(a));
    snprintf(meta, sizeof(meta), "%s%s%s", b.ms ? a : "earlier",
             b.ms ? " ago" : "", verified ? "" : " · unverified");
    if (!verified) tint = UiTheme::kWarn;
  }
  lv_obj_t* m = lv_label_create(box);
  lv_label_set_text(m, meta);
  lv_obj_set_style_text_color(m, lv_color_hex(tint), 0);
  lv_obj_align_to(m, t, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
  lv_obj_set_height(box, LV_SIZE_CONTENT);
}

uint32_t inboundStamp() { return Rns::Inbox::newest(); }

uint32_t outboundStamp() {
  RnsTransport::OutMessage o[8];
  const size_t n = RnsTransport::lxmfOutbound(o, 8);
  uint32_t h = (uint32_t)n;
  for (size_t i = 0; i < n; i++) h ^= o[i].queuedMs ^ (o[i].sentMs * 31u);
  return h;
}

void threadRebuild() {
  if (!sThreadCol || !lv_obj_is_valid(sThreadCol)) return;
  lv_obj_clean(sThreadCol);

  static Bubble b[24];
  size_t n = 0;

  struct Ctx { Bubble* b; size_t* n; const uint8_t* dest; };
  Ctx ctx{b, &n, sThreadDest};
  Rns::Inbox::readPage(0, 16, [](const Rns::InboxRecord& r, void* vp) {
    Ctx* c = (Ctx*)vp;
    if (*c->n >= 20 || memcmp(r.from, c->dest, 16) != 0) return;
    Bubble& x = c->b[(*c->n)++];
    x.ours = false;
    // Another boot's millis is a foreign clock: those records sort oldest
    // and say "earlier" instead of wearing an age computed across runs.
    x.ms = (r.bootId == Rns::Inbox::bootId()) ? r.bootMs : 0;
    const size_t tn = r.textLen < 80 ? r.textLen : 80;
    memcpy(x.text, r.text, tn); x.text[tn] = 0;
    x.sentMs = 0; x.ok = false;
    x.standing = r.standing;
  }, &ctx);

  RnsTransport::OutMessage o[8];
  const size_t on = RnsTransport::lxmfOutbound(o, 8);
  for (size_t i = 0; i < on && n < 24; i++) {
    if (memcmp(o[i].dest, sThreadDest, 16) != 0) continue;
    Bubble& x = b[n++];
    x.ours = true;
    x.ms = o[i].queuedMs;
    snprintf(x.text, sizeof(x.text), "%s", o[i].text);
    x.sentMs = o[i].sentMs;
    x.ok = o[i].ok;
  }

  // Oldest at the top, the way a conversation reads.
  for (size_t i = 0; i + 1 < n; i++)
    for (size_t j = 0; j + 1 < n - i; j++)
      if (b[j].ms > b[j + 1].ms) { Bubble t = b[j]; b[j] = b[j + 1]; b[j + 1] = t; }

  if (!n) lv_list_add_text(sThreadCol, "nothing said yet");
  for (size_t i = 0; i < n; i++) bubbleRow(sThreadCol, b[i]);
  lv_obj_scroll_to_y(sThreadCol, LV_COORD_MAX, LV_ANIM_OFF);
  sThreadStamp = outboundStamp();
}

void threadTick(lv_timer_t*) {
  if (!sThreadCol || !lv_obj_is_valid(sThreadCol)) return;
  // Both directions: the outbound log for our fates, the inbox for their
  // words — a reply that arrived used to stay invisible until the operator
  // backed out and returned.
  static uint32_t lastIn = 0;
  const uint32_t in = inboundStamp();
  if (outboundStamp() != sThreadStamp || in != lastIn) {
    lastIn = in;
    threadRebuild();
  }
}

void sendCurrent(const char* text) {
  if (!text || !*text) { Ui::toast("nothing to send"); return; }
  Ui::toast(RnsTransport::queueLxmfReply(sThreadDest, text)
            ? "queued" : "could not queue — transport down?");
  threadRebuild();
}

// --- quick messages ---------------------------------------------------------

void openQuick(lv_event_t*) {
  lv_obj_t* body = Ui::newScreen("Quick send");

  struct Canned { const char* text; bool distress; };
  static const Canned kRows[] = {
    { "On my way",          false },
    { "Arrived, all clear", false },
    { "Holding position",   false },
    { "POSITION",           false },     // replaced with coordinates at send
    { "Need assistance",    true  },
  };
  for (const Canned& c : kRows) {
    lv_obj_t* btn = lv_button_create(body);
    UiTheme::actionButton(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 40);
    lv_obj_t* l = lv_label_create(btn);
    if (c.distress) {
      lv_label_set_text(l, "Need assistance — HOLD 2 s");
      lv_obj_set_style_text_color(l, lv_color_hex(UiTheme::kBad), 0);
    } else if (strcmp(c.text, "POSITION") == 0) {
      lv_label_set_text(l, "Send my position");
    } else {
      lv_label_set_text(l, c.text);
    }
    lv_obj_center(l);
    if (c.distress) {
      Ui::onHeld2s(btn, [](void*) {
        sendCurrent("EMERGENCY: need assistance");
        Ui::back();
      }, nullptr);
    } else {
      lv_obj_add_event_cb(btn, [](lv_event_t* e) {
        lv_obj_t* lbl = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
        const char* t = lv_label_get_text(lbl);
        char pos[64];
        if (strcmp(t, "Send my position") == 0) {
#if HAS_GPS
          const Gps::Fix f = Gps::fix();
          if (!f.valid) { Ui::toast("no fix to send"); return; }
          snprintf(pos, sizeof(pos), "POS %.5f %.5f alt %.0f m", f.latitude, f.longitude,
                   (double)f.altitude);
          t = pos;
#else
          Ui::toast("no receiver"); return;
#endif
        }
        sendCurrent(t);
        Ui::back();
      }, LV_EVENT_CLICKED, nullptr);
    }
  }
  lv_obj_t* note = lv_label_create(body);
  lv_label_set_text(note, "One tap, no keyboard, minimal airtime.");
  lv_obj_set_style_text_color(note, lv_color_hex(UiTheme::kInkLabel), 0);
  Ui::push(lv_obj_get_parent(lv_obj_get_parent(body)));
}

// --- the thread screen ------------------------------------------------------

void openThreadScreen(const uint8_t from[16]) {
  memcpy(sThreadDest, from, 16);
  char title[34];
  Ui::peerLabel(from, title, sizeof(title));
  lv_obj_t* body = Ui::newScreen(title);

  sThreadCol = lv_obj_create(body);
  lv_obj_remove_style_all(sThreadCol);
  lv_obj_set_width(sThreadCol, lv_pct(100));
  lv_obj_set_flex_grow(sThreadCol, 1);
  lv_obj_set_flex_flow(sThreadCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sThreadCol, 4, 0);
  lv_obj_set_scroll_dir(sThreadCol, LV_DIR_VER);

  lv_obj_t* row = lv_obj_create(body);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 46);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 4, 0);
  sThreadTa = Ui::textarea(row, "Tap to write…", true, false);
  lv_obj_set_flex_grow(sThreadTa, 1);
  lv_obj_t* send = lv_button_create(row);
  UiTheme::actionButton(send);
  lv_obj_set_size(send, 56, lv_pct(100));
  lv_obj_t* sl = lv_label_create(send);
  lv_label_set_text(sl, LV_SYMBOL_UP);
  lv_obj_center(sl);
  lv_obj_add_event_cb(send, [](lv_event_t*) {
    sendCurrent(lv_textarea_get_text(sThreadTa));
    lv_textarea_set_text(sThreadTa, "");
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* quick = lv_button_create(row);
  UiTheme::actionButton(quick);
  lv_obj_set_size(quick, 64, lv_pct(100));
  lv_obj_t* ql = lv_label_create(quick);
  lv_label_set_text(ql, "QUICK");
  lv_obj_center(ql);
  lv_obj_add_event_cb(quick, openQuick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  lv_timer_t* t = lv_timer_create(threadTick, 2000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sThreadCol = nullptr;
  }, LV_EVENT_DELETE, t);

  threadRebuild();
  Ui::push(scr);
}

// --- the conversations list -------------------------------------------------

struct Thread { uint8_t from[16]; char preview[40]; uint8_t count; };

void listRebuild() {
  lv_obj_clean(sList);
  static Thread th[16];
  size_t n = 0;

  struct Ctx { Thread* t; size_t* n; };
  Ctx ctx{th, &n};
  Rns::Inbox::readPage(0, 16, [](const Rns::InboxRecord& r, void* vp) {
    Ctx* c = (Ctx*)vp;
    for (size_t i = 0; i < *c->n; i++)
      if (memcmp(c->t[i].from, r.from, 16) == 0) { c->t[i].count++; return; }
    if (*c->n >= 16) return;
    Thread& t = c->t[(*c->n)++];
    memcpy(t.from, r.from, 16);
    t.count = 1;
    const size_t tn = r.textLen < 34 ? r.textLen : 34;
    snprintf(t.preview, sizeof(t.preview), "%.*s", (int)tn, r.text);
  }, &ctx);

  if (!n) { lv_list_add_text(sList, "no messages yet"); return; }
  // The rows' destinations live and die with this screen: a shared static
  // here once let a stacked older list open the wrong peer's thread and
  // queue a reply to the wrong destination.
  uint8_t* dests = (uint8_t*)lv_malloc(n * 16);
  if (!dests) return;
  lv_obj_add_event_cb(sList, [](lv_event_t* e) {
    lv_free(lv_event_get_user_data(e));
  }, LV_EVENT_DELETE, dests);
  for (size_t i = 0; i < n; i++) {
    char line[96], who[34];
    Ui::peerLabel(th[i].from, who, sizeof(who));
    snprintf(line, sizeof(line), "%s · %u\n%s", who, th[i].count, th[i].preview);
    lv_obj_t* btn = lv_list_add_button(sList, LV_SYMBOL_ENVELOPE, line);
    memcpy(dests + i * 16, th[i].from, 16);
    lv_obj_add_event_cb(btn, [](lv_event_t* e) {
      openThreadScreen((const uint8_t*)lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, dests + i * 16);
  }
}

} // namespace

namespace Ui {

void peerLabelHex(const char* hashHex, char* out, size_t n) {
  Neighbor nb = {};
  if (neighbors.byHash(hashHex, nb) && nb.name[0]) { snprintf(out, n, "%s", nb.name); return; }
  if (PeerNames::lookup(hashHex, out, n) && out[0]) return;
  snprintf(out, n, "%.8s", hashHex);
}

void peerLabel(const uint8_t hash[16], char* out, size_t n) {
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
  peerLabelHex(hex, out, n);
}

void openThread(const uint8_t from[16]) { openThreadScreen(from); }

void openMessages() {
  lv_obj_t* body = newScreen("Messages");
  sList = lv_list_create(body);
  lv_obj_set_width(sList, lv_pct(100));
  lv_obj_set_flex_grow(sList, 1);
  listRebuild();
  lv_obj_t* scr = lv_obj_get_parent(lv_obj_get_parent(body));
  // Arrivals refresh the open list; before this, a message that came in
  // while the screen was showing stayed invisible until reopening.
  lv_timer_t* t = lv_timer_create([](lv_timer_t*) {
    if (!sList || !lv_obj_is_valid(sList)) return;
    static uint32_t lastIn = 0;
    const uint32_t in = inboundStamp();
    if (in != lastIn) { lastIn = in; listRebuild(); }
  }, 2000, nullptr);
  lv_obj_add_event_cb(scr, [](lv_event_t* e) {
    lv_timer_delete((lv_timer_t*)lv_event_get_user_data(e));
    sList = nullptr;
  }, LV_EVENT_DELETE, t);
  push(scr);
}

} // namespace Ui

#endif // HAS_LVGL_UI
