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
#include <esp_rtc_time.h>
#include <esp_system.h>

namespace Diag {

// Placed in RTC RAM and deliberately not zeroed at startup, so what the run
// that just died wrote is still here: its length, and the marks a deliberate
// restart left on its way out. A power cut or brownout drops the RTC domain
// and leaves this as noise, which the magic detects.
struct RtcRecord { uint32_t magic; uint32_t uptimeS; RestartMarks restart; };
RTC_NOINIT_ATTR static RtcRecord sRtc;

static const uint32_t kRtcMagic = 0x52544D32;   // "RTM2": the record grew the restart marks

static Boot sBoot;

// Every task this firmware creates, in the order main.cpp starts them, plus
// the two the framework brings. Keeping the list here rather than in the
// heartbeat is the point: the log and the API cannot disagree about which
// tasks are watched, which is how the GNSS task came to be missing from one
// of them while carrying the smallest stack of the lot.
static const char* const kTasks[] = {
  "loopTask",     // Arduino: the heartbeat and scheduled restarts
  "async_udp",    // AsyncUDP: the captive resolver's callbacks (CaptiveDns.h)
  "radio",        // LoRa RX/TX
  "display",      // panel + button
  "rns",          // everything inside microReticulum
  "autoif",       // AutoInterface peering
  "sdcard",       // hot-plug polling
  "gps",          // NMEA parsing
  "async_tcp",    // ESPAsyncWebServer
  "ppp-uart",     // the bridge UART reader (PppUart.h)
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
    case ESP_RST_USB:       return "USB peripheral";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lock-up (double exception)";
    default:                return "unknown";
  }
}

void begin() {
  const esp_reset_reason_t r = esp_reset_reason();
  sBoot.reason     = (uint8_t)r;
  sBoot.reasonName = resetReasonName((uint8_t)r);
  // A deliberate restart is not a fault; everything else is worth a warning.
  // The two a flashing tool causes count as deliberate: esptool resets the
  // chip over the USB peripheral at the end of every upload on a native-USB
  // board (ESP_RST_USB) and over JTAG on a debugger's. Without them a normal
  // flash reported "previous run ended: unknown" and was counted unclean,
  // which is the log saying a crash where there was a tool doing its job.
  sBoot.clean = (r == ESP_RST_POWERON || r == ESP_RST_EXT ||
                 r == ESP_RST_SW      || r == ESP_RST_DEEPSLEEP ||
                 r == ESP_RST_USB     || r == ESP_RST_JTAG);

  if (sRtc.magic == kRtcMagic) {
    sBoot.prevUptimeKnown = true;
    sBoot.prevUptimeS     = sRtc.uptimeS;
    if (sRtc.restart.entryMs) {
      const RestartMarks m = sRtc.restart;
      sBoot.lastRestart.toPersistMs = m.persistMs ? m.persistMs - m.entryMs : 0;
      sBoot.lastRestart.toBootMs    = rtcMs() - (m.persistMs ? m.persistMs : m.entryMs);
      sBoot.lastRestart.known       = true;
    }
  }
  sRtc.magic   = kRtcMagic;
  sRtc.uptimeS = 0;
  sRtc.restart = RestartMarks{0, 0};

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
  if (sBoot.lastRestart.known)
    log_i("last restart: %lu ms to the core's persist-restart, %lu ms from there to this boot",
          (unsigned long)sBoot.lastRestart.toPersistMs, (unsigned long)sBoot.lastRestart.toBootMs);
}

const Boot& boot() { return sBoot; }

bool startTask(TaskFunction_t fn, const char* name, uint32_t stackBytes,
               void* arg, UBaseType_t priority, BaseType_t core) {
  if (xTaskCreatePinnedToCore(fn, name, stackBytes, arg, priority, nullptr, core) == pdPASS)
    return true;
  // The figures that decide it: a task stack must come from byte-addressable
  // internal RAM, so MALLOC_CAP_INTERNAL alone overstates what is available —
  // part of it is 32-bit-only IRAM a stack cannot use. Reporting the 8-bit
  // largest block is the difference between "40 KB free and an 8 KB stack
  // failed, which makes no sense" and the actual answer.
  log_e("task \"%s\" (%lu B stack) could not be created: %lu B free / %lu B largest block "
        "of 8-bit internal RAM (%lu B free internal in all) — this node is running without it",
        name, (unsigned long)stackBytes,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return false;
}

// The bill, in 8-bit internal RAM: the origin costStart() took, and where the
// last cost() left it.
static uint32_t sCostOrigin = 0;
static uint32_t sCostMark   = 0;

static uint32_t freeDram() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void costStart() { sCostOrigin = sCostMark = freeDram(); }

void cost(const char* what) {
  const uint32_t now = freeDram();
  // Signed: a subsystem that hands memory back — a probe that finds nothing
  // and frees what it took to look — is worth seeing as a credit rather than
  // as an unsigned number the size of the address space.
  const long spent = (long)sCostMark - (long)now;
  const long total = (long)sCostOrigin - (long)now;
  log_i("cost: %-14s %+7ld B  (%lu free, %lu largest, %+ld B since boot)",
        what, spent, (unsigned long)now,
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        total);
  sCostMark = now;
}

void tick(uint32_t uptimeS) { sRtc.uptimeS = uptimeS; }

RestartMarks& restartMarks() { return sRtc.restart; }
uint32_t rtcMs() { return (uint32_t)(esp_rtc_get_time_us() / 1000); }

Heap heap() {
  Heap h;
  h.freeInternal    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  h.minFreeInternal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  h.largestBlock    = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const uint32_t dram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  h.freeDram         = heap_caps_get_free_size(dram);
  h.minFreeDram      = heap_caps_get_minimum_free_size(dram);
  h.largestDramBlock = heap_caps_get_largest_free_block(dram);
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
  log_i("dram: %lu free (min %lu, largest block %lu) — what a stack can use",
        (unsigned long)h.freeDram, (unsigned long)h.minFreeDram, (unsigned long)h.largestDramBlock);
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
  // Whichever of the two is tighter, because either one can be the binding
  // constraint and only one of them is ever the reason an allocation failed.
  // The internal figure counts 32-bit-only IRAM as well, so on a board where
  // that is a large share of it — a classic ESP32 — it reads tens of
  // kilobytes healthier than the byte-addressable heap a stack must come
  // from, and a threshold on it alone stays quiet through exactly the
  // shortage that stops another task being created (Diag.h). On a board where
  // the two coincide this is the same one warning it always was.
  const uint32_t tightest = h.minFreeDram < h.minFreeInternal ? h.minFreeDram : h.minFreeInternal;
  if (tightest < DIAG_HEAP_WARN_B) {
    log_w("%s heap fell to %lu bytes at its lowest (warning below %d)%s",
          h.minFreeDram < h.minFreeInternal ? "byte-addressable" : "internal",
          (unsigned long)tightest, DIAG_HEAP_WARN_B,
          h.minFreeDram < h.minFreeInternal ? " — this is what a stack or buffer can use" : "");
    warned = true;
  }
  return warned;
}

} // namespace Diag
