// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  UiTheme.h — the instrument theme, said once
//
//  The palette and styles from the design spec (roadmap/design/
//  v4-ui-design-spec.md): deep navy grounds, a blue-gray ink ramp, and
//  three semantic colours with strict meanings — green is locked/verified,
//  amber is marginal/in-transit, and red is reserved for failure and
//  distress alone. Every screen asks this module; no file names a colour
//  of its own.
// ============================================================================
#pragma once

#include "Ui.h"

#if HAS_LVGL_UI

// The design's own faces, converted from its font files: Barlow for the
// body and the big readings, IBM Plex Mono for values and hashes, Barlow
// Condensed for the caps labels.
LV_FONT_DECLARE(font_barlow_16);
LV_FONT_DECLARE(font_barlow_28);
LV_FONT_DECLARE(font_plexmono_16);
LV_FONT_DECLARE(font_condensed_14);

namespace UiTheme {

// Grounds, darkest to lightest.
constexpr uint32_t kGround    = 0x0f1820;  // the screen itself
constexpr uint32_t kGround2   = 0x14202b;  // bars, keyboard
constexpr uint32_t kSurface   = 0x1a2836;  // cards, rows
constexpr uint32_t kSurfaceHi = 0x22384c;  // pressed / selected
constexpr uint32_t kEdge      = 0x2c455d;  // borders, accent edges

// Ink ramp.
constexpr uint32_t kInk       = 0xeaf0f6;  // primary text
constexpr uint32_t kInkDim    = 0x9ebbd8;  // secondary
constexpr uint32_t kInkLabel  = 0x6f8ba4;  // caps field labels
constexpr uint32_t kAccent    = 0x5980a6;  // interactive accent

// Semantics — the meanings are part of the design, not a suggestion.
constexpr uint32_t kGood      = 0x6cc08b;  // locked, verified, usable
constexpr uint32_t kWarn      = 0xe0a83f;  // drift, marginal, in transit
constexpr uint32_t kBad       = 0xe0656f;  // failed, stale, distress only

void init(lv_display_t* disp);           // global dark theme + shared styles
void screen(lv_obj_t* scr);              // ground + primary ink
void bar(lv_obj_t* obj);                 // status / action bar lane
void card(lv_obj_t* obj);                // a reading's surface
void labelCaps(lv_obj_t* label);         // the small field label above a value
void value(lv_obj_t* label);             // the value itself
void actionButton(lv_obj_t* btn);        // one slot of the bottom action bar

} // namespace UiTheme

#endif // HAS_LVGL_UI
