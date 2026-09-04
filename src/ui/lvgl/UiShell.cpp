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
//  UiShell.cpp — buffers, input, the status bar, the keyboard, navigation
//
//  The mobile chrome lives here. The status bar is the phone convention the
//  operator already knows: who I am on the left, how I am doing on the right
//  — radio, Wi-Fi, GNSS, battery, and the clock once the receiver has set it.
//  It sits on the top layer, so every screen inherits it without carrying it.
// ============================================================================
#include "Ui.h"
#include "UiTheme.h"

#if HAS_LVGL_UI

#include <Arduino.h>
#include <esp_heap_caps.h>
#include "TftPanel.h"
#include "TouchInput.h"
#include "Keypad.h"
#include "Settings.h"
#include "Power.h"
#include "Gps.h"
#include "WifiManager.h"

// ---------------------------------------------------------------------------
// LVGL's allocator: PSRAM first at every size, internal RAM only as the
// fallback. lv_conf.h says why this exists; these are the five functions the
// LV_STDLIB_CUSTOM contract asks for. Widget memory has no DMA or ISR needs,
// so external RAM is simply correct for all of it.
// ---------------------------------------------------------------------------
extern "C" {
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
void* lv_malloc_core(size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}
void lv_free_core(void* p) { heap_caps_free(p); }
void* lv_realloc_core(void* p, size_t new_size) {
  void* q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}
}

namespace {

TftPanel* sPanel = nullptr;

constexpr int32_t kBarH = 22;

// One partial render buffer, ~1/10 of the panel, in internal DRAM:
// DMA-friendly, and the PSRAM-over-SPI chunking trap never applies. One,
// deliberately: the flush below blits synchronously and reports ready before
// returning, so a second buffer never overlapped anything — it only held
// 15 KiB of the internal RAM this file's own allocator notes call scarce.
constexpr size_t kBufPx = DISPLAY_WIDTH * 32;
static uint8_t sBuf1[kBufPx * 2];

lv_obj_t* sKeyboard = nullptr;
lv_obj_t* sBar = nullptr;
lv_obj_t* sBarName = nullptr;
lv_obj_t* sBarIcons = nullptr;

// The stack under the active screen. Six is deeper than any path here goes.
lv_obj_t* sStack[6];
uint8_t   sDepth = 0;

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const uint32_t n = (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);
  lv_draw_sw_rgb565_swap(px, n);              // ST7789 eats big-endian RGB565
  sPanel->blitArea((int16_t)area->x1, (int16_t)area->y1,
                   (int16_t)area->x2, (int16_t)area->y2, px);
  lv_display_flush_ready(disp);
}

volatile bool sTouchSeen = false;
volatile bool sSwallow = false;          // the wake tap must not press anything
uint8_t sRot = 0;                        // quarter turns, mirrored in the panel's MADCTL

void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  const TouchInput::Point p = TouchInput::poll();
  if (sSwallow) {
    // The contact that woke the panel is still on the glass: report it as
    // released until it truly lifts, so waking cannot also press whatever
    // happened to be under the finger.
    if (!p.down) sSwallow = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  data->state = p.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (p.down) {
    // The glass reports portrait-native points; the UI lives in the turned
    // frame, so the point turns the same way the MADCTL did.
    int16_t x = p.x, y = p.y;
    switch (sRot) {
      case 1: x = p.y;                                  y = (int16_t)(DISPLAY_WIDTH  - 1 - p.x); break;
      case 2: x = (int16_t)(DISPLAY_WIDTH  - 1 - p.x);  y = (int16_t)(DISPLAY_HEIGHT - 1 - p.y); break;
      case 3: x = (int16_t)(DISPLAY_HEIGHT - 1 - p.y);  y = p.x;                                 break;
    }
    data->point.x = x; data->point.y = y; sTouchSeen = true;
  }
}

#if HAS_KEYPAD || HAS_TRACKBALL
// The physical keys, as an LVGL keypad device. Its group holds the text fields
// the shell creates, so a key goes to whichever one the operator last touched —
// which is the same field the on-glass keyboard would have been typing into.
lv_group_t* sKeys = nullptr;

void keypadCb(lv_indev_t*, lv_indev_data_t* data) {
  // The keyboard hands over one key per read and has no concept of a release:
  // its controller latches what was pressed and forgets it. LVGL wants a press
  // and then a release, so each key is held for exactly one pass and let go on
  // the next. Typing faster than the shell polls is bounded by the poll rate,
  // not lost — the controller's own latch queues the rest.
  static uint32_t held = 0;
  if (held) { data->key = held; data->state = LV_INDEV_STATE_RELEASED; held = 0; return; }

  const uint8_t k = Keypad::read();
  if (k == Keypad::KEY_NONE) { data->state = LV_INDEV_STATE_RELEASED; return; }

  uint32_t lk;
  switch (k) {
    // Up and down walk the controls rather than moving inside one. LVGL moves
    // focus around a group on NEXT and PREV and sends the plain arrows to
    // whatever already has it — so a device with no pointer and only arrows
    // can reach nothing at all. On the board that has no touch layer this is
    // the difference between a usable node and an ornament, and on the one
    // that has both a trackball and glass it is what the ball should do
    // anyway. Left and right stay arrows, so a text cursor still moves.
    case Keypad::KEY_UP:        lk = LV_KEY_PREV;      break;
    case Keypad::KEY_DOWN:      lk = LV_KEY_NEXT;      break;
    case Keypad::KEY_LEFT:      lk = LV_KEY_LEFT;      break;
    case Keypad::KEY_RIGHT:     lk = LV_KEY_RIGHT;     break;
    case Keypad::KEY_ENTER:     lk = LV_KEY_ENTER;     break;   // 0x0D here, 0x0A there
    case Keypad::KEY_BACKSPACE: lk = LV_KEY_BACKSPACE; break;
    case Keypad::KEY_ESC:       lk = LV_KEY_ESC;       break;
    default:
      // Anything printable is itself. Anything else is a key this firmware has
      // no meaning for — a controller's own function key — and is dropped
      // rather than typed as a control character into a message.
      if (k < 0x20 || k > 0x7E) { data->state = LV_INDEV_STATE_RELEASED; return; }
      lk = k;
      break;
  }
  data->key = lk;
  data->state = LV_INDEV_STATE_PRESSED;
  held = lk;
  sTouchSeen = true;                     // a keypress is activity, like a tap
}
#endif

// --- the status bar ---------------------------------------------------------

const char* batterySymbol(const Power::Battery& b) {
  if (!b.present)      return LV_SYMBOL_BATTERY_EMPTY;
  if (b.percent >= 85) return LV_SYMBOL_BATTERY_FULL;
  if (b.percent >= 60) return LV_SYMBOL_BATTERY_3;
  if (b.percent >= 35) return LV_SYMBOL_BATTERY_2;
  if (b.percent >= 10) return LV_SYMBOL_BATTERY_1;
  return LV_SYMBOL_BATTERY_EMPTY;
}

void barTick(lv_timer_t*) {
  char text[96];
  size_t n = 0;
  auto add = [&](const char* s) {
    n += snprintf(text + n, sizeof(text) - n, "%s", s);
    if (n >= sizeof(text)) n = sizeof(text) - 1;   // snprintf reports, not writes
  };

  // The clock, once the receiver has set it: hours and minutes are the part a
  // person wants; the date belongs to the About dialog.
#if HAS_GPS
  const Gps::Fix f = Gps::fix();
  if (f.clockSet && strlen(f.utc) >= 16) {
    char hm[6] = { f.utc[11], f.utc[12], f.utc[13], f.utc[14], f.utc[15], 0 };
    add(hm); add("  ");
  }
  if (f.valid) { add(LV_SYMBOL_GPS); add(" "); }
#endif
  if (settings.links().wifiEnabled) { add(LV_SYMBOL_WIFI); add(" "); }
  add(g_stats.radioOnline ? "LoRa " : "");
  const Power::Battery b = Power::battery();
  if (b.chargeKnown && b.charging) add(LV_SYMBOL_CHARGE);
  add(batterySymbol(b));
  if (b.present) { char p[8]; snprintf(p, sizeof(p), " %u%%", b.percent); add(p); }

  // The daylight toggle reaches the glass here, whichever door set it —
  // the settings form or the console funnel.
  if (settings.display().daylight != UiTheme::daylight()) Ui::retheme();

  // Unchanged text is not re-set: LVGL reallocates and repaints on every
  // set, and this bar changes at most once a minute.
  if (strcmp(lv_label_get_text(sBarIcons), text)) lv_label_set_text(sBarIcons, text);
  if (strcmp(lv_label_get_text(sBarName), wifiManager.ssid()))
    lv_label_set_text(sBarName, wifiManager.ssid());
}

void barCreate() {
  sBar = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sBar);
  lv_obj_set_size(sBar, lv_pct(100), kBarH);   // pct, so rotation re-fits it
  lv_obj_align(sBar, LV_ALIGN_TOP_MID, 0, 0);
  UiTheme::bar(sBar);
  lv_obj_set_style_pad_hor(sBar, 6, 0);

  sBarName = lv_label_create(sBar);
  lv_obj_set_style_text_color(sBarName, lv_color_hex(UiTheme::kInkDim), 0);
  lv_obj_align(sBarName, LV_ALIGN_LEFT_MID, 0, 0);

  sBarIcons = lv_label_create(sBar);
  lv_obj_set_style_text_color(sBarIcons, lv_color_hex(UiTheme::kInkDim), 0);
  lv_obj_align(sBarIcons, LV_ALIGN_RIGHT_MID, 0, 0);

  lv_timer_create(barTick, 1000, nullptr);
  barTick(nullptr);
}

// --- the keyboard -----------------------------------------------------------

// The active screen's form (newScreen tags it on the screen's user data):
// padded while the keyboard is up so the last field can scroll clear of the
// keys, and only then — the padding used to be permanent, and every screen
// paid it whether or not a keyboard was showing.
void formKeyboardPad(int32_t px) {
  lv_obj_t* body = (lv_obj_t*)lv_obj_get_user_data(lv_screen_active());
  if (body && lv_obj_is_valid(body)) lv_obj_set_style_pad_bottom(body, px, 0);
}

void kbEvent(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
    formKeyboardPad(0);
  }
}

void taFocusEvent(lv_event_t* e) {
  lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
#if HAS_KEYPAD || HAS_TRACKBALL
    // A board with keys on it does not need half its screen given over to a
    // picture of keys. The field is scrolled into view and left alone; the
    // keypad device is already aimed at it, because focus is what aims it.
    if (Keypad::present()) { lv_obj_scroll_to_view(ta, LV_ANIM_ON); return; }
#endif
    const bool numeric = (bool)(uintptr_t)lv_event_get_user_data(e);
    lv_keyboard_set_mode(sKeyboard, numeric ? LV_KEYBOARD_MODE_NUMBER
                                            : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(sKeyboard, ta);
    lv_obj_remove_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sKeyboard);
    // A modal dialog is lifted clear of the keys; a field on an ordinary
    // screen scrolls clear instead — its form has the bottom padding for it.
    lv_obj_t* dlg = ta;
    lv_obj_t* up  = lv_obj_get_parent(dlg);
    while (up && up != lv_layer_top() && up != lv_screen_active()) {
      dlg = up;
      up = lv_obj_get_parent(dlg);
    }
    if (up == lv_layer_top() && dlg != sKeyboard) {
      lv_obj_t* box = dlg;
      for (lv_obj_t* m = ta; m && m != dlg; m = lv_obj_get_parent(m))
        if (lv_obj_get_parent(m) == dlg) { box = m; break; }
      const int32_t kbTop = LV_VER_RES - lv_obj_get_height(sKeyboard);
      lv_obj_set_style_max_height(box, kbTop - kBarH - 8, 0);
      lv_obj_align(box, LV_ALIGN_TOP_MID, 0, kBarH + 2);
    } else {
      // An ordinary screen scrolls its field clear of the keys instead of
      // being lifted, which needs headroom below the last field.
      formKeyboardPad(140);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
  } else if (code == LV_EVENT_DEFOCUSED) {
    lv_keyboard_set_textarea(sKeyboard, nullptr);
    lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
    formKeyboardPad(0);
  }
}

void backEvent(lv_event_t*) { Ui::back(); }

} // namespace

namespace Ui {

bool shellInit(TftPanel& panel) {
  sPanel = &panel;
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  lv_display_t* disp = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!disp) return false;
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_set_buffers(disp, sBuf1, nullptr, sizeof(sBuf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  UiTheme::init(disp);                   // before any widget exists
  // Which way up the board is built, before any chrome is laid out: the bar
  // and the tabs size themselves in percentages of a resolution that has to be
  // the final one. A board with an accelerometer moves on from here; a board
  // without one stays, and without this stayed in the controller's portrait on
  // glass mounted landscape.
  setRotation(DISPLAY_ROTATION);
  UiTheme::setDaylight(settings.display().daylight);
  // text_font is an inherited style, and overlays live on the top layer —
  // outside any themed screen's inheritance. Without this, their labels
  // fall back to LV_FONT_DEFAULT, which lacks the design's delimiters: the
  // idle clock drew its middle dots as hollow rectangles.
  lv_obj_set_style_text_font(lv_layer_top(), &font_barlow_16, 0);
  lv_obj_set_style_text_color(lv_layer_top(), lv_color_hex(UiTheme::kInk), 0);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touchCb);

#if HAS_KEYPAD || HAS_TRACKBALL
  // Only where keys actually answered. A board with a keyboard fitted but
  // silent keeps the on-glass one and stays usable, rather than presenting a
  // group nothing can move focus around.
  if (Keypad::present()) {
    sKeys = lv_group_create();
    lv_indev_t* keys = lv_indev_create();
    lv_indev_set_type(keys, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keys, keypadCb);
    lv_indev_set_group(keys, sKeys);
    // Every focusable widget the shell builds from here joins this group on
    // its own: LVGL adds a new object to the default group when its class asks
    // to be focusable, which buttons, lists and text fields all do. That is
    // what makes the whole shell reachable from the keys rather than only the
    // text fields — and it is the difference between a usable node and an
    // ornament on the one board here that has no touch layer at all.
    lv_group_set_default(sKeys);
  }
#endif

  barCreate();

  sKeyboard = lv_keyboard_create(lv_layer_top());
  lv_obj_set_style_bg_color(sKeyboard, lv_color_hex(UiTheme::kGround2), 0);
  lv_obj_set_height(sKeyboard, 132);     // four ~33 px rows: tappable, compact
  lv_keyboard_set_mode(sKeyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_event_cb(sKeyboard, kbEvent, LV_EVENT_ALL, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);

  // One real frame before setup() continues: the boot splash. The display
  // task is not running yet, so this paints synchronously and holds the
  // glass until openHome replaces (and deletes) it.
  {
    lv_obj_t* splash = lv_obj_create(nullptr);
    UiTheme::screen(splash);
    lv_obj_t* name = lv_label_create(splash);
    lv_label_set_text(name, "RETIMESH");
    lv_obj_set_style_text_font(name, &font_barlow_28, 0);
    lv_obj_set_style_text_letter_space(name, 3, 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t* fw = lv_label_create(splash);
    lv_label_set_text_fmt(fw, "%s · lvgl %d.%d", FW_VERSION,
                          lv_version_major(), lv_version_minor());
    lv_obj_set_style_text_color(fw, lv_color_hex(UiTheme::kInkDim), 0);
    lv_obj_align(fw, LV_ALIGN_CENTER, 0, 4);
    lv_obj_t* bd = lv_label_create(splash);
    lv_label_set_text(bd, BOARD_NAME);
    lv_obj_set_style_text_color(bd, lv_color_hex(UiTheme::kInkLabel), 0);
    lv_obj_align(bd, LV_ALIGN_CENTER, 0, 26);
    lv_obj_t* st = lv_label_create(splash);
    lv_label_set_text_fmt(st, "LXMF · %s", settings.radio().region[0]
                          ? settings.radio().region : "region unset");
    lv_obj_set_style_text_color(st, lv_color_hex(UiTheme::kInkLabel), 0);
    lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_screen_load(splash);
    lv_refr_now(disp);
  }

  log_i("gui: LVGL %d.%d.%d shell up — status bar, %u B render buffer",
        lv_version_major(), lv_version_minor(), lv_version_patch(),
        (unsigned)sizeof(sBuf1));
  return true;
}

uint32_t shellLoop() { return lv_timer_handler(); }

void setRotation(uint8_t quarterTurns) {
  const uint8_t want = quarterTurns & 3;
  if (want == sRot) return;
  lv_display_t* d = lv_display_get_default();
  if (!d || !sPanel) return;
  sRot = want;
  // The controller turns the frame (TftPanel::setRotation); LVGL is told
  // only the logical resolution and the pct-sized chrome re-fits itself.
  // LVGL's own rotation stays at zero deliberately: in this version it
  // hands the flush turned coordinates and expects the driver to rotate
  // every buffer in software — the mangled panel that taught us so.
  sPanel->setRotation(want);
  const bool sideways = want & 1;
  lv_display_set_resolution(d, sideways ? DISPLAY_HEIGHT : DISPLAY_WIDTH,
                               sideways ? DISPLAY_WIDTH  : DISPLAY_HEIGHT);
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
}

// Whether a finger has touched the glass since this was last asked. The
// display's blanking timer consumes it: the shell reads the touch layer, so
// without this the timer only ever heard the physical buttons and blanked
// the panel under an operator mid-navigation — the bench found it inside a
// minute.
bool consumeTouch() {
  const bool t = sTouchSeen;
  sTouchSeen = false;
  return t;
}

void swallowTouch() { sSwallow = true; }

void setLabel(lv_obj_t* label, const char* text) {
  if (!label || !lv_obj_is_valid(label)) return;
  if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void formatKm(char* out, size_t n, double km) {
  snprintf(out, n, km < 10.0 ? "%.2f km" : "%.1f km", km);
}

void ageTextMs(uint32_t sinceMs, char* out, size_t n) {
  ageTextS(sinceMs / 1000, out, n);
}

void ageTextS(uint32_t s, char* out, size_t n) {
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

void onHeld2s(lv_obj_t* btn, void (*fire)(void*), void* userData) {
  // LONG_PRESSED at the indev's 400 ms, then repeats every ~100 ms: sixteen
  // repeats is the two-second bar. A resistive panel in rain produces stray
  // taps, never a stray hold — which is why the irreversible actions live
  // behind exactly this and nothing shorter.
  struct Held { void (*fire)(void*); void* ud; uint8_t count; };
  Held* h = (Held*)lv_malloc(sizeof(Held));
  h->fire = fire; h->ud = userData; h->count = 0;
  lv_obj_add_event_cb(btn, [](lv_event_t* e) {
    Held* hh = (Held*)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) hh->count = 0;
    else if (code == LV_EVENT_LONG_PRESSED_REPEAT && ++hh->count == 16) hh->fire(hh->ud);
    else if (code == LV_EVENT_DELETE) lv_free(hh);
  }, LV_EVENT_ALL, h);
}

void groupedHash(const char* hex, char* out, size_t n) {
  size_t w = 0;
  for (size_t i = 0; hex[i] && w < n - 3; i++) {
    out[w++] = hex[i];
    if ((i % 4) == 3 && hex[i + 1]) out[w++] = (i == 15) ? '\n' : ' ';
  }
  out[w] = 0;
}

void push(lv_obj_t* screen) {
  if (sDepth >= sizeof(sStack) / sizeof(sStack[0])) {
    // A refusal owns the screen it refuses: silently dropping the pointer
    // leaked a fully built tree per refused push.
    lv_obj_delete(screen);
    toast("too deep — go back first");
    return;
  }
  sStack[sDepth++] = lv_screen_active();
  lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 150, 0, false);
}

void back() {
  if (!sDepth) return;
  // The keyboard outlives every screen, so it must not keep pointing into
  // one about to be freed: a hardware back while a field was focused left it
  // bound to a deleted textarea, and the next key wrote into freed memory.
  // Deleting an object sends no DEFOCUSED, so the unbinding happens here.
  lv_keyboard_set_textarea(sKeyboard, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  // auto_del deletes the screen being left once the slide is over.
  lv_screen_load_anim(sStack[--sDepth], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 150, 0, true);
}

bool atRoot() { return sDepth == 0; }

void retheme() {
  UiTheme::setDaylight(settings.display().daylight);
  // The shared styles refresh in place, but colors baked into widgets at
  // creation outlive them, so the shell rebuilds from the ground: the
  // stacked screens die with their colors and home is born in the new
  // ones. The keyboard unbinds first, as in back() — the textarea it
  // points into is about to be freed.
  lv_keyboard_set_textarea(sKeyboard, nullptr);
  lv_obj_add_flag(sKeyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(sKeyboard, lv_color_hex(UiTheme::kGround2), 0);
  lv_obj_set_style_text_color(lv_layer_top(), lv_color_hex(UiTheme::kInk), 0);
  lv_obj_set_style_text_color(sBarName, lv_color_hex(UiTheme::kInkDim), 0);
  lv_obj_set_style_text_color(sBarIcons, lv_color_hex(UiTheme::kInkDim), 0);
  while (sDepth) {
    lv_obj_t* s = sStack[--sDepth];
    if (s && lv_obj_is_valid(s)) lv_obj_delete(s);
  }
  resetIdle();
  openHome();   // deletes the active screen, builds home in the new palette
}

// The one place that knows how deep a body sits in its screen, so pages
// stop re-deriving "the grandparent" by hand at every push and teardown.
lv_obj_t* screenOf(lv_obj_t* body) { return lv_obj_get_parent(lv_obj_get_parent(body)); }

lv_obj_t* newScreen(const char* title) {
  lv_obj_t* scr = lv_obj_create(nullptr);
  UiTheme::screen(scr);
  lv_obj_set_style_pad_top(scr, kBarH, 0);   // the status bar's lane

  lv_obj_t* col = lv_obj_create(scr);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, lv_pct(100), lv_pct(100));
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(col, 4, 0);
  lv_obj_set_style_pad_row(col, 4, 0);

  if (title) {
    lv_obj_t* head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_t* backBtn = lv_button_create(head);
    lv_obj_set_size(backBtn, 40, 28);
    lv_obj_align(backBtn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* bl = lv_label_create(backBtn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(backBtn, backEvent, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* tl = lv_label_create(head);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_font(tl, &font_barlow_16, 0);
    lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);
  }

  lv_obj_t* body = lv_obj_create(col);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 4, 0);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  // The keyboard's clearance is paid only while the keyboard is up (the
  // focus handler pads this body, found again through the screen's user
  // data). A standing reservation here cost every screen 140 px of height —
  // a list ended mid-screen with dead glass below it.
  lv_obj_set_user_data(scr, body);
  return body;
}

void toast(const char* text) {
  lv_obj_t* t = lv_label_create(lv_layer_top());
  lv_label_set_text(t, text);
  lv_obj_set_style_bg_opa(t, LV_OPA_80, 0);
  lv_obj_set_style_bg_color(t, lv_color_black(), 0);
  lv_obj_set_style_text_color(t, lv_color_white(), 0);
  lv_obj_set_style_pad_all(t, 6, 0);
  lv_obj_set_width(t, LV_SIZE_CONTENT);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, kBarH + 2);
  lv_obj_delete_delayed(t, 2500);
}

lv_obj_t* textarea(lv_obj_t* parent, const char* placeholder,
                   bool oneLine, bool numeric) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, oneLine);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_FOCUSED, (void*)(uintptr_t)numeric);
  lv_obj_add_event_cb(ta, taFocusEvent, LV_EVENT_DEFOCUSED, nullptr);
  return ta;
}

} // namespace Ui

#endif // HAS_LVGL_UI
