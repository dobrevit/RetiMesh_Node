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

/* The C library's malloc, deliberately: this firmware routes allocations
 * above a small threshold to PSRAM (PSRAM_MALLOC_THRESHOLD in Config.h), so
 * LVGL's widgets land in the 2 MB that would otherwise sit idle, and the
 * library's own pool allocator would only hide that from the heap report. */
#define LV_USE_STDLIB_MALLOC   LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING   LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF  LV_STDLIB_CLIB

/* No OS bindings: every LVGL call is made from the display task, which is the
 * same single-writer discipline the page stack has always used. */
#define LV_USE_OS LV_OS_NONE

/* The default theme sized for a 240x320 at this dot pitch. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
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
