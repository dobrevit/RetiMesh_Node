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

#include <new>
#include <exception>
#include <atomic>

#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_rtc_time.h>
#include <esp_system.h>

namespace Diag {

// Placed in RTC RAM and deliberately not zeroed at startup, so what the run
// that just died wrote is still here: its length, the marks a deliberate
// restart left on its way out, and what it had failed to allocate. A power cut
// or brownout drops the RTC domain and leaves this as noise, which the magic
// detects. The layout and both decisions made about it are in BootRecord.h,
// where the host tests can reach them.
RTC_NOINIT_ATTR static Record sRtc;

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
  "audio",        // the I2S speaker's note player (Buzzer.cpp)
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

// --- running out of memory, survivably (Diag.h) ------------------------------
static std::atomic<uint32_t> sAllocFailures{0};
static std::atomic<uint32_t> sAllocLastMs{0};
static std::atomic<uint32_t> sCaught{0};
// A node in an allocation storm must not drown its own diagnosis: the first
// failure is worth a line, the next thousand are worth a count.
static uint32_t sLastAllocLogMs = 0;
static constexpr uint32_t kAllocLogEveryMs = 5000;

// Copy the live counts into the record that outlives the run. The atomics stay
// authoritative — a count kept only in RTC memory would be incremented by every
// task that can fail an allocation, and this has to be callable from a
// new-handler, where taking a lock or allocating is not an option.
//
// Two tasks can land here at once. That is safe and deliberately not guarded:
// each store is a single aligned 32-bit write of a value read from a
// monotonically increasing atomic, so the worst interleaving writes a count
// that was true a moment ago, and the next tick() corrects it. What must never
// happen — a torn value, or a count going backwards past what the record
// already holds — cannot, because nothing here reads the record to compute
// what to write.
static inline void mirrorFaults() {
  sRtc.allocFailures = sAllocFailures.load(std::memory_order_relaxed);
  sRtc.caught        = sCaught.load(std::memory_order_relaxed);
}

Faults faults() {
  Faults f;
  f.allocFailures = sAllocFailures.load();
  f.lastMs        = sAllocLastMs.load();
  f.caught        = sCaught.load();
  return f;
}

void noteCaught(const char* what, const char* why) {
  sCaught.fetch_add(1, std::memory_order_relaxed);
  mirrorFaults();
  const uint32_t dram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  log_e("%s failed and was contained: %s — %lu B free / %lu B largest block of 8-bit "
        "internal RAM. That work was skipped; the node is still running",
        what, why,
        (unsigned long)heap_caps_get_free_size(dram),
        (unsigned long)heap_caps_get_largest_free_block(dram));
}

// Called by the runtime the moment any allocation fails — inside this
// firmware, inside the framework, inside a library, wherever. It cannot make
// the allocation succeed; what it can do is put the reason on the record
// before whatever throws next, so a failure downstream is explained rather
// than mysterious. Throwing bad_alloc from here is one of the three things a
// new-handler is permitted to do, and it is what the default would have done.
static void onAllocationFailed() {
  sAllocFailures.fetch_add(1, std::memory_order_relaxed);
  const uint32_t now = millis();
  sAllocLastMs.store(now, std::memory_order_relaxed);
  // Before the throw, not after: what this failure leads to may be the death
  // that stops tick() ever running again, and then this store is the only
  // record that the node was short of memory at all.
  mirrorFaults();
  if (now - sLastAllocLogMs >= kAllocLogEveryMs || sAllocFailures.load() == 1) {
    sLastAllocLogMs = now;
    const uint32_t dram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    log_e("an allocation failed (%lu since boot): %lu B free / %lu B largest block of "
          "8-bit internal RAM. Something is about to fail or be refused",
          (unsigned long)sAllocFailures.load(),
          (unsigned long)heap_caps_get_free_size(dram),
          (unsigned long)heap_caps_get_largest_free_block(dram));
  }
  throw std::bad_alloc();
}

// The last word, for a throw in code this firmware does not own and cannot
// wrap — serveStatic reads the filesystem on the async_tcp task, and neither
// the task nor the code is ours. The node still dies here. It dies saying
// why, which a bare backtrace did not.
static void onTerminate() {
  const uint32_t dram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const char* why = "an exception nobody caught";
  if (auto p = std::current_exception()) {
    try { std::rethrow_exception(p); }
    catch (const std::bad_alloc&) { why = "an allocation failed and nobody caught it"; }
    catch (const std::exception& e) { why = e.what(); }
    catch (...) {}
  }
  log_e("about to abort: %s. %lu B free / %lu B largest block of 8-bit internal RAM, "
        "%lu allocation failures since boot. This is the node running out of the memory "
        "a buffer can actually use, not a logic fault",
        why,
        (unsigned long)heap_caps_get_free_size(dram),
        (unsigned long)heap_caps_get_largest_free_block(dram),
        (unsigned long)sAllocFailures.load());
  // The last thing this run does that the next one can read. abort() raises a
  // panic, the panic ends the run, and RTC memory is what survives it.
  mirrorFaults();
  Serial.flush();
  abort();
}

void begin() {
  // The record first — before the handlers, not just before the claim.
  // onAllocationFailed() mirrors into sRtc, so from the moment it is installed
  // any failed allocation overwrites the dead run's counts. claimRun() reads
  // and claims in one call so the two cannot be sequenced wrongly here, and
  // doing it before the handlers exist means there is no window at all.
  const Previous prev = claimRun(sRtc);

  std::set_new_handler(onAllocationFailed);
  std::set_terminate(onTerminate);
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

  if (prev.known) {
    sBoot.prevUptimeKnown   = true;
    sBoot.prevUptimeS       = prev.uptimeS;
    sBoot.prevAllocFailures = prev.allocFailures;
    sBoot.prevCaught        = prev.caught;
    if (prev.restartMarked) {
      const RestartTiming t = restartTiming(prev.restart, rtcMs());
      sBoot.lastRestart.toPersistMs = t.toPersistMs;
      sBoot.lastRestart.toBootMs    = t.toBootMs;
      sBoot.lastRestart.known       = t.known;
    }
  }

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

  // The line this whole change exists to make possible. An unclean boot whose
  // previous run had been failing allocations is a node that died of memory,
  // and until now that could only be guessed at from a heap curve sampled
  // minutes earlier. Said at warning level next to the reason, because it is
  // the explanation for it; silent when the run before was clean, so the
  // absence of this line means something too.
  if (sBoot.prevUptimeKnown && (sBoot.prevAllocFailures || sBoot.prevCaught))
    log_w("the run that just ended had %lu allocation failure(s) and contained %lu — "
          "it was short of memory before it stopped",
          (unsigned long)sBoot.prevAllocFailures, (unsigned long)sBoot.prevCaught);
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

// Every pass of the main loop, which is also the cadence the run length needs.
// Mirroring here rather than only at the moment of failure is what covers the
// ordinary case: a node whose allocations failed an hour before it finally died
// of something else still reports them.
void tick(uint32_t uptimeS) {
  sRtc.uptimeS = uptimeS;
  mirrorFaults();
}

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
