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
//  Bootloader.cpp — see Bootloader.h.
//
//  The download-mode transition. ESP32-S2, S3 and C3 have a bit in
//  RTC_CNTL_OPTION1_REG, FORCE_DOWNLOAD_BOOT, that the ROM reads after a
//  reset in place of the strapping pins: set, the ROM goes into its serial
//  downloader as if BOOT had been held. It is written from a shutdown
//  handler, which is the sequence the Arduino core's own
//  usb_persist_restart(RESTART_BOOTLOADER) performs. Note the order the
//  handlers run in: esp_restart() walks the table from the last slot down,
//  so a handler registered late runs *early* — before esp_wifi_stop, not
//  after it. That is harmless for a register write nothing later touches,
//  but it is the opposite of what an earlier comment here claimed. The
//  register name is the SoC header's, so a chip without the bit does not
//  compile the path at all rather than writing something else.
//
//  The handler is registered when the request is accepted, not when the
//  restart happens, because registration can fail: IDF keeps five slots and
//  Wi-Fi already holds one. A failure at restart time could only be logged,
//  by which time the host tool had already been told 202 and was waiting for
//  a downloader that would never appear. At request time it is a refusal
//  with a reason.
//
//  The downloader that comes up talks on whichever port the ROM uses: the
//  USB-Serial/JTAG peripheral on a native-USB S3, UART0 through the CP2102
//  on a Heltec V3. The host tooling waits for that port. Nothing persists
//  across the reset except the bit itself, which the ROM clears, so a power
//  cycle at any point simply boots the application again — there is no
//  state to get stuck in.
// ============================================================================
#include "Bootloader.h"

#include <esp_system.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>
#if HAS_USB_NCM
  #include "esp32-hal-tinyusb.h"
  #include "UsbNcm.h"
#endif
#include "Config.h"
#include "Diag.h"

namespace Bootloader {

static Sequencer sSeq;

// Timestamps a restart leaves for the next boot to read, in RTC memory and
// on the RTC clock, both of which survive every reset short of a power
// cycle — the ROM session and esptool's reset between the two runs included.
#include <esp_attr.h>
#include <esp_private/esp_clk.h>
extern "C" uint64_t esp_rtc_get_time_us(void);
struct RestartMarks { uint32_t magic; uint32_t entryMs, persistMs, handlersMs; };
static RTC_NOINIT_ATTR RestartMarks sMarks;
static constexpr uint32_t kMarksMagic = 0x52535452;   // "RSTR"
static LastRestart sLast = {0, 0, 0, false};
static inline uint32_t rtcMs() { return (uint32_t)(esp_rtc_get_time_us() / 1000); }
static void IRAM_ATTR markHandlers() { if (sMarks.magic == kMarksMagic) sMarks.handlersMs = rtcMs(); }

void begin() {
  if (sMarks.magic == kMarksMagic && sMarks.entryMs) {
    const uint32_t now = rtcMs();
    sLast.toPersistMs  = sMarks.persistMs  ? sMarks.persistMs - sMarks.entryMs : 0;
    sLast.toHandlersMs = sMarks.handlersMs ? sMarks.handlersMs - (sMarks.persistMs ? sMarks.persistMs : sMarks.entryMs) : 0;
    sLast.toBootMs     = now - (sMarks.handlersMs ? sMarks.handlersMs : sMarks.entryMs);
    sLast.known = true;
    log_i("last restart: %lu ms to the core's persist-restart, %lu ms to the last shutdown handler, %lu ms to this boot",
          (unsigned long)sLast.toPersistMs, (unsigned long)sLast.toHandlersMs, (unsigned long)sLast.toBootMs);
  }
  sMarks.magic = 0;
  // Registered first, so it runs last: shutdown handlers run in reverse order.
  esp_register_shutdown_handler(markHandlers);
}
LastRestart lastRestart() { return sLast; }
static const char kRecovery[] =
  "hold BOOT, press RST (or replug USB while holding BOOT), then flash with esptool";

#if defined(RTC_CNTL_FORCE_DOWNLOAD_BOOT)
  #define HAS_FORCE_DOWNLOAD_BOOT 1
#else
  #define HAS_FORCE_DOWNLOAD_BOOT 0
#endif

Caps caps() {
  Caps c;
  c.forceDownloadBoot = HAS_FORCE_DOWNLOAD_BOOT;
  c.nativeUsb         = BOARD_USB_NATIVE;
  c.otgStack          = HAS_USB_NCM;
  c.bridgeAutoReset   = BOARD_BRIDGE_AUTO_RESET;
  return c;
}

#if HAS_FORCE_DOWNLOAD_BOOT
static void IRAM_ATTR forceDownloadBootHandler() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}
#endif

// Arm the ROM for download mode on the next reset. True when the handler is
// in place — including when it already was: IDF refuses a second
// registration of the same function as an invalid state, which here means
// the work is done. Nothing caches that; IDF is the record.
static bool armDownloadBoot() {
  #if HAS_USB_NCM
    return true;                      // restart() takes the core's path instead
  #elif HAS_FORCE_DOWNLOAD_BOOT
    const esp_err_t err = esp_register_shutdown_handler(forceDownloadBootHandler);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return true;
    log_e("bootloader: esp_register_shutdown_handler failed (%d): the shutdown table is full", (int)err);
    return false;
  #else
    return false;
  #endif
}

static void disarmDownloadBoot() {
  #if HAS_FORCE_DOWNLOAD_BOOT
    esp_unregister_shutdown_handler(forceDownloadBootHandler);
  #endif
}

Plan plan() { return Bootloader::plan(caps()); }
bool canEnterAutomatically() { return Bootloader::canEnterAutomatically(caps()); }
const char* manualRecovery() { return kRecovery; }

Refusal request(Target target, Source source, uint32_t delayMs, const char** whyNot) {
  if (target == Target::Bootloader && !canEnterAutomatically()) {
    if (whyNot) *whyNot = whyNotAutomatic(caps());
    return Refusal::CannotEnter;
  }
  // The sequencer decides first. An earlier version armed the ROM before
  // asking, and a request the sequencer then refused — because a plain
  // reboot was already going through — left the handler registered, so that
  // reboot landed in the downloader with nobody waiting for it. Arming after
  // acceptance, and disarming if the arming itself fails, leaves a refusal
  // with nothing behind it.
  if (!sSeq.request(target, source, delayMs, millis())) {
    if (whyNot) *whyNot = "a restart is already in progress";
    return Refusal::Busy;
  }
  if (target == Target::Bootloader && !armDownloadBoot()) {
    // Accepted a moment ago and now impossible: the armed restart would run
    // as a plain reboot, which is not what was asked. There is no way to
    // un-accept, so it is left armed as a reboot and reported honestly.
    if (whyNot) *whyNot = "cannot arm the ROM downloader: the shutdown handler table is full; the node will restart into the application instead";
    return Refusal::CannotArm;
  }
  log_w("restart requested: into the %s, by %s, in %lu ms",
        targetName(target), sourceName(source), (unsigned long)delayMs);
  return Refusal::None;
}

bool    pending()  { return sSeq.pending(); }
Pending snapshot() { return sSeq.snapshot(); }

static void quiesce() {
  // From here on RetiTransportServer refuses connections and the settings
  // handlers refuse writes (they check pending()). What is in flight is left
  // to finish; the delay before this step was the time for that.
  log_w("restarting into the %s", targetName(sSeq.snapshot().target));
  // Record the run length now: tick() in loop() would have, but the restart
  // happens on this pass and the previous run's length is how the next boot
  // tells a deliberate restart from a crash loop.
  Diag::tick(millis() / 1000);
  Serial.flush();
  // NVS writes are transactional and the Reticulum store checkpoints itself;
  // LittleFS is not unmounted here on purpose. The transport task may be in a
  // write, and pulling the filesystem from under it would turn a clean restart
  // into a panic — after which no shutdown handler runs and a bootloader
  // request would silently become a plain reboot.
  delay(50);
}

static void restart() {
  const Target target = sSeq.snapshot().target;
  sMarks = { kMarksMagic, rtcMs(), 0, 0 };
  #if HAS_USB_NCM
    // The core's own way into the downloader on the composite device: it
    // registers a shutdown handler that sets the download bit, hands the
    // USB peripheral back to the serial-JTAG unit, and restarts. Our own
    // handler stays unarmed on this path — armDownloadBoot() defers to it.
    if (target == Target::Bootloader) {
      UsbNcm::detach();
      sMarks.persistMs = rtcMs();
      usb_persist_restart(RESTART_BOOTLOADER);
      return;
    }
  #endif
  // A plain restart must not carry a download flag armed by an earlier
  // request that was then outranked.
  if (target != Target::Bootloader) disarmDownloadBoot();
  esp_restart();
}

void tick() {
  switch (sSeq.tick(millis())) {
    case Step::None:    break;
    case Step::Quiesce: quiesce(); break;
    case Step::Restart: restart(); break;
  }
}

} // namespace Bootloader
