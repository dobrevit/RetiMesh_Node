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
//  handler so that it is the last thing done before the reset, after every
//  other handler has run, and it is exactly the sequence the Arduino core's
//  usb_persist_restart(RESTART_BOOTLOADER) performs for its own 1200-baud
//  touch. The register name is the SoC header's, so a chip without the bit
//  does not compile the path at all rather than writing something else.
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
  c.usbCdcOtg         = BOARD_USB_CDC_OTG;       // the firmware-owned CDC; 0 until the composite device exists
  c.bridgeAutoReset   = BOARD_BRIDGE_AUTO_RESET;
  return c;
}

Plan plan() { return Bootloader::plan(caps()); }
bool canEnterAutomatically() { return Bootloader::canEnterAutomatically(caps()); }
const char* manualRecovery() { return kRecovery; }

bool request(Target target, Source source, uint32_t delayMs, const char** whyNot) {
  if (target == Target::Bootloader && !canEnterAutomatically()) {
    if (whyNot) *whyNot = "this chip cannot enter its downloader from software; use the bridge's auto-reset or hold BOOT";
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

#if HAS_FORCE_DOWNLOAD_BOOT
static void IRAM_ATTR forceDownloadBootHandler() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
}
#endif

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
  if (sSeq.target() == Target::Bootloader) {
    #if HAS_FORCE_DOWNLOAD_BOOT
      esp_register_shutdown_handler(forceDownloadBootHandler);
    #endif
  }
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
