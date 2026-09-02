/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
 *
 * LVGL configuration — only the overrides. Everything not set here takes the
 * library's own default through lv_conf_internal.h, which is the point: a
 * full copy of the 1000-line template is a thousand lines that can drift from
 * the version they were copied at.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* RGB565, the panel's native depth. Byte order is handled in the flush
 * callback (lv_draw_sw_rgb565_swap) because the ST7789 wants big-endian. */
#define LV_COLOR_DEPTH 16

/* A custom allocator (UiShell.cpp), PSRAM-first at every size. The C
 * library's malloc only prefers PSRAM above the firmware's 128-byte
 * threshold, and LVGL allocates mostly *below* it — styles, event nodes,
 * small structs — so a form build filled internal DRAM and starved the
 * allocations that may only live there: the flash driver's bounce buffer
 * and newlib's lazily-created locks. That combination took the node down
 * from the RNS task while the GUI was merely opening a settings form
 * (coredump, 2026-09-02). The GUI now takes its memory from the 2 MB that
 * sit idle, at every size, and touches internal RAM only when PSRAM is
 * genuinely full. */
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING   LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_CLIB

/* No OS bindings: every LVGL call is made from the display task, which is the
 * same single-writer discipline the page stack has always used. */
#define LV_USE_OS LV_OS_NONE

/* The default theme sized for a 240x320 at this dot pitch. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 0  // nothing sets it; flash is metered here
#define LV_FONT_MONTSERRAT_28 1  // the home board's clock — the one big face
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Diagnostics off in release builds; the node's own logging carries what
 * matters, and LVGL's is chatty. */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* Not used, and each costs flash. */
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0     /* the node has its own QR generator */
#define LV_USE_BARCODE 0
#define LV_USE_TJPGD 0
#define LV_USE_LODEPNG 0
#define LV_USE_FFMPEG 0
#define LV_USE_SYSMON 0
#define LV_USE_PROFILER 0

#endif /* LV_CONF_H */
