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

// The palette is runtime state, not constants: the daylight theme (the
// design's variant D) swaps every slot while the geometry stays put. Call
// sites read the slots each time they paint, so a rebuilt screen is born
// in whichever palette is current. The roles keep the night values' names.

// Grounds, darkest to lightest (in the night palette's terms).
extern uint32_t kGround;     // the screen itself
extern uint32_t kGround2;    // bars, keyboard
extern uint32_t kSurface;    // cards, rows
extern uint32_t kSurfaceHi;  // pressed / selected
extern uint32_t kEdge;       // borders, accent edges

// Ink ramp.
extern uint32_t kInk;        // primary text
extern uint32_t kInkDim;     // secondary
extern uint32_t kInkLabel;   // caps field labels
extern uint32_t kAccent;     // interactive accent

// Semantics — the meanings are part of the design, not a suggestion.
extern uint32_t kGood;       // locked, verified, usable
extern uint32_t kWarn;       // drift, marginal, in transit
extern uint32_t kBad;        // failed, stale, distress only

void init(lv_display_t* disp);           // global theme + shared styles
void setDaylight(bool on);               // swaps the palette, restyles the shared styles
bool daylight();                         // which palette is on the glass
void screen(lv_obj_t* scr);              // ground + primary ink
void bar(lv_obj_t* obj);                 // status / action bar lane
void card(lv_obj_t* obj);                // a reading's surface
void labelCaps(lv_obj_t* label);         // the small field label above a value
void value(lv_obj_t* label);             // the value itself
void actionButton(lv_obj_t* btn);        // one slot of the bottom action bar
// The spec's reading row — caps label left, bright value right — returned
// as the value label. Four screens carried four drifting copies.
lv_obj_t* reading(lv_obj_t* parent, const char* label, const char* value);

} // namespace UiTheme

#endif // HAS_LVGL_UI
