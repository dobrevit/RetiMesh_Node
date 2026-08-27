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

#include "Diag.h"

#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

namespace Diag {

// Placed in RTC RAM and deliberately not zeroed at startup, so the value
// written by the run that just died is still here. A power cut or brownout
// drops the RTC domain and leaves this as noise, which the magic detects.
RTC_NOINIT_ATTR static uint32_t sRtcMagic;
RTC_NOINIT_ATTR static uint32_t sRtcUptimeS;

static const uint32_t kRtcMagic = 0x52544D31;   // "RTM1"

static Boot sBoot;

// Every task this firmware creates, in the order main.cpp starts them, plus
// the two the framework brings. Keeping the list here rather than in the
// heartbeat is the point: the log and the API cannot disagree about which
// tasks are watched, which is how the GNSS task came to be missing from one
// of them while carrying the smallest stack of the lot.
static const char* const kTasks[] = {
  "loopTask",     // Arduino: the heartbeat and scheduled restarts
  "dns",          // captive-portal DNS
  "radio",        // LoRa RX/TX
  "display",      // panel + button
  "rns",          // everything inside microReticulum
  "autoif",       // AutoInterface peering
  "sdcard",       // hot-plug polling
  "gps",          // NMEA parsing
  "async_tcp",    // ESPAsyncWebServer
};

const char* resetReasonName(uint8_t reason) {
  switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic or unhandled exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

void begin() {
  const esp_reset_reason_t r = esp_reset_reason();
  sBoot.reason     = (uint8_t)r;
  sBoot.reasonName = resetReasonName((uint8_t)r);
  // A deliberate restart is not a fault; everything else is worth a warning.
  sBoot.clean = (r == ESP_RST_POWERON || r == ESP_RST_EXT ||
                 r == ESP_RST_SW      || r == ESP_RST_DEEPSLEEP);

  if (sRtcMagic == kRtcMagic) {
    sBoot.prevUptimeKnown = true;
    sBoot.prevUptimeS     = sRtcUptimeS;
  }
  sRtcMagic   = kRtcMagic;
  sRtcUptimeS = 0;

  // One small NVS write per boot. This is the counter that tells a node which
  // has been up all week apart from one that has quietly been restarting.
  Preferences p;
  if (p.begin(DIAG_NVS_NAMESPACE, false)) {
    sBoot.count = p.getUInt("boots", 0) + 1;
    p.putUInt("boots", sBoot.count);
    p.end();
  }

  char ran[48] = "";
  if (sBoot.prevUptimeKnown)
    snprintf(ran, sizeof(ran), " after %luh%02lum%02lus",
             (unsigned long)(sBoot.prevUptimeS / 3600),
             (unsigned long)(sBoot.prevUptimeS % 3600 / 60),
             (unsigned long)(sBoot.prevUptimeS % 60));

  if (sBoot.clean) {
    log_i("boot #%lu — %s%s", (unsigned long)sBoot.count, sBoot.reasonName, ran);
  } else {
    // The RTC value is lost when the rail drops, so its absence next to a
    // brownout or a panic says the node lost power rather than crashed.
    log_w("boot #%lu — previous run ended: %s%s%s", (unsigned long)sBoot.count,
          sBoot.reasonName, ran,
          sBoot.prevUptimeKnown ? "" : " (run length lost: the RTC domain was not held up)");
  }
}

const Boot& boot() { return sBoot; }

void tick(uint32_t uptimeS) { sRtcUptimeS = uptimeS; }

Heap heap() {
  Heap h;
  h.freeInternal    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  h.minFreeInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  h.largestBlock    = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  h.freePsram       = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  return h;
}

size_t taskCount() { return sizeof(kTasks) / sizeof(kTasks[0]); }

size_t stacks(TaskStack* out, size_t max) {
  size_t n = 0;
  for (const char* name : kTasks) {
    if (n >= max) break;
    TaskHandle_t h = xTaskGetHandle(name);
    out[n].name     = name;
    out[n].present  = (h != nullptr);
    // ESP-IDF's FreeRTOS returns this in bytes, unlike vanilla FreeRTOS.
    out[n].headroom = h ? (uint32_t)uxTaskGetStackHighWaterMark(h) : 0;
    n++;
  }
  return n;
}

uint32_t lowestHeadroom(const char** name) {
  TaskStack st[16];
  const size_t n = stacks(st, sizeof(st) / sizeof(st[0]));
  uint32_t lowest = UINT32_MAX;
  const char* who = nullptr;
  for (size_t i = 0; i < n; i++) {
    if (!st[i].present) continue;
    if (st[i].headroom < lowest) { lowest = st[i].headroom; who = st[i].name; }
  }
  // nullptr, not "none": zero headroom is the most urgent reading there is, so
  // the caller has to be able to tell it apart from having found no task at all.
  if (name) *name = who;
  return who ? lowest : 0;
}

bool report() {
  const Heap h = heap();
  TaskStack st[16];
  const size_t n = stacks(st, sizeof(st) / sizeof(st[0]));

  // One line, only the tasks that exist: a build without a GNSS receiver or an
  // SD card should not print zeros for tasks it never started.
  char line[192];
  size_t off = 0;
  for (size_t i = 0; i < n && off < sizeof(line); i++) {
    if (!st[i].present) continue;
    off += snprintf(line + off, sizeof(line) - off, " %s %lu",
                    st[i].name, (unsigned long)st[i].headroom);
  }
  log_i("stack headroom:%s", off ? line : " (no tasks)");

  // The gap between free and largest-block is the fragmentation: an allocator
  // with 60 KB free and a 4 KB largest block will fail an 8 KB request while
  // looking healthy on the free figure alone.
  log_i("heap: %lu free (min %lu, largest block %lu) psram %lu free",
        (unsigned long)h.freeInternal, (unsigned long)h.minFreeInternal,
        (unsigned long)h.largestBlock, (unsigned long)h.freePsram);

  bool warned = false;
  const char* lowest = nullptr;
  const uint32_t headroom = lowestHeadroom(&lowest);
  if (lowest && headroom < DIAG_STACK_WARN_B) {
    log_w("stack headroom on task \"%s\" is down to %lu bytes — it is the one that will "
          "trip the canary, and the reset that follows names it only on the console",
          lowest, (unsigned long)headroom);
    warned = true;
  }
  if (h.minFreeInternal < DIAG_HEAP_WARN_B) {
    log_w("internal heap fell to %lu bytes at its lowest (warning below %d)",
          (unsigned long)h.minFreeInternal, DIAG_HEAP_WARN_B);
    warned = true;
  }
  return warned;
}

} // namespace Diag
