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
#include "Config.h"
#include "Diag.h"

namespace Bootloader {

static Sequencer sSeq;
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
  c.bridgeAutoReset   = BOARD_BRIDGE_AUTO_RESET;
  return c;
}

#if HAS_FORCE_DOWNLOAD_BOOT
static void IRAM_ATTR forceDownloadBootHandler() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}
static bool sHandlerRegistered = false;
#endif

// Arm the ROM for download mode on the next reset. True when the handler is
// in place (or already was: a second registration of the same function is
// refused by IDF as an invalid state, which is not a failure here).
static bool armDownloadBoot() {
  #if HAS_FORCE_DOWNLOAD_BOOT
    if (sHandlerRegistered) return true;
    const esp_err_t err = esp_register_shutdown_handler(forceDownloadBootHandler);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) { sHandlerRegistered = true; return true; }
    log_e("bootloader: esp_register_shutdown_handler failed (%d): the shutdown table is full", (int)err);
    return false;
  #else
    return false;
  #endif
}

Plan plan() { return Bootloader::plan(caps()); }
bool canEnterAutomatically() { return Bootloader::canEnterAutomatically(caps()); }
const char* manualRecovery() { return kRecovery; }

bool request(Target target, Source source, uint32_t delayMs, const char** whyNot) {
  if (target == Target::Bootloader && !canEnterAutomatically()) {
    if (whyNot) *whyNot = BOARD_USB_NATIVE
      ? "software entry hangs a native-USB board (its USB unit survives the reset); esptool's DTR/RTS handshake does it instead"
      : "this chip cannot enter its downloader from software; use the bridge's auto-reset or hold BOOT";
    return false;
  }
  // Armed before the sequencer accepts, so a refusal here leaves nothing
  // pending; the bit itself is only consulted by the ROM after a reset.
  if (target == Target::Bootloader && !armDownloadBoot()) {
    if (whyNot) *whyNot = "cannot arm the ROM downloader: the shutdown handler table is full; reboot and retry, or hold BOOT";
    return false;
  }
  const bool ok = sSeq.request(target, source, delayMs, millis());
  if (!ok && whyNot) *whyNot = "a restart is already in progress";
  if (ok) log_w("restart requested: into the %s, by %s, in %lu ms",
                targetName(target), sourceName(source), (unsigned long)delayMs);
  return ok;
}

bool   pending() { return sSeq.pending(); }
State  state()   { return sSeq.state(); }
Target target()  { return sSeq.target(); }

static void quiesce() {
  // From here on RetiTransportServer refuses connections and the settings
  // handlers refuse writes (they check pending()). What is in flight is left
  // to finish; the delay before this step was the time for that.
  log_w("restarting into the %s", targetName(sSeq.target()));
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
  // The download-boot handler, if this restart wants one, was registered at
  // request time (see armDownloadBoot); there is nothing left to arm.
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
