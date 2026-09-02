// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// UiTheme.cpp — see UiTheme.h. Styles are static and shared: LVGL styles are
// references, so a hundred widgets pointing at one style cost one style.
#include "UiTheme.h"

#if HAS_LVGL_UI

namespace UiTheme {
namespace {
lv_style_t sCard, sCaps, sValue, sBar, sAction, sActionPressed;
} // namespace

void init(lv_display_t* disp) {
  // The default theme in dark mode carries the widgets we do not restyle —
  // switches, dropdowns, msgboxes — with our accent as its primary.
  // The Montserrat built-ins stay compiled only as the symbol bank: the
  // design fonts carry no icon glyphs, so LV_SYMBOL_* falls through — the
  // fallback pointers are set statically in the generated font files,
  // because those structs live in flash and a runtime write to them is a
  // panic (the bench found that as a boot loop with no console).
  lv_theme_default_init(disp, lv_color_hex(kAccent), lv_color_hex(kGood),
                        true /* dark */, &font_barlow_16);

  lv_style_init(&sCard);
  lv_style_set_bg_color(&sCard, lv_color_hex(kSurface));
  lv_style_set_bg_opa(&sCard, LV_OPA_COVER);
  lv_style_set_border_color(&sCard, lv_color_hex(kEdge));
  lv_style_set_border_width(&sCard, 1);
  lv_style_set_radius(&sCard, 4);

  lv_style_init(&sCaps);
  lv_style_set_text_color(&sCaps, lv_color_hex(kInkLabel));
  lv_style_set_text_font(&sCaps, &font_condensed_14);
  lv_style_set_text_letter_space(&sCaps, 1);

  lv_style_init(&sValue);
  lv_style_set_text_color(&sValue, lv_color_hex(kInk));
  lv_style_set_text_font(&sValue, &font_plexmono_16);   // readings are instrument voice

  lv_style_init(&sBar);
  lv_style_set_bg_color(&sBar, lv_color_hex(kGround2));
  lv_style_set_bg_opa(&sBar, LV_OPA_COVER);

  lv_style_init(&sAction);
  lv_style_set_bg_color(&sAction, lv_color_hex(kSurface));
  lv_style_set_bg_opa(&sAction, LV_OPA_COVER);
  lv_style_set_radius(&sAction, 4);
  lv_style_set_text_color(&sAction, lv_color_hex(kInkDim));
  lv_style_set_shadow_width(&sAction, 0);

  lv_style_init(&sActionPressed);
  lv_style_set_bg_color(&sActionPressed, lv_color_hex(kSurfaceHi));
  lv_style_set_text_color(&sActionPressed, lv_color_hex(kInk));
}

void screen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(kGround), 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(kInk), 0);
}

void bar(lv_obj_t* obj)          { lv_obj_add_style(obj, &sBar, 0); }
void card(lv_obj_t* obj)         { lv_obj_add_style(obj, &sCard, 0); }
void labelCaps(lv_obj_t* label)  { lv_obj_add_style(label, &sCaps, 0); }
void value(lv_obj_t* label)      { lv_obj_add_style(label, &sValue, 0); }

lv_obj_t* reading(lv_obj_t* parent, const char* label, const char* value) {
  lv_obj_t* row = lv_obj_create(parent);
  card(row);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(row, 8, 0);
  lv_obj_set_style_pad_ver(row, 5, 0);
  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, label);
  labelCaps(l);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* v = lv_label_create(row);
  UiTheme::value(v);
  lv_label_set_text(v, value ? value : "—");
  lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
  return v;
}

void actionButton(lv_obj_t* btn) {
  lv_obj_add_style(btn, &sAction, 0);
  lv_obj_add_style(btn, &sActionPressed, LV_STATE_PRESSED);
}

} // namespace UiTheme
#endif // HAS_LVGL_UI
