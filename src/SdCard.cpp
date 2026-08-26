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
//  SdCard.cpp — see SdCard.h
// ============================================================================
#include "SdCard.h"
#include <sd_diskio.h>

SdCard sdCard;

static const char* kMount = "/sd";
static const char* kLogDir = "/retimesh";          // SD paths are relative to the mount point
static const char* kLogFile = SdCard::LOG_PATH;

const char* SdCard::stateName(State s) {
  switch (s) {
    case State::Mounted:     return "mounted";
    case State::Partial:     return "partial";
    case State::Unformatted: return "unformatted";
    case State::Formatting:  return "formatting";
    case State::Error:       return "error";
    default:                 return "absent";
  }
}

void SdCard::begin() {
  _lock = xSemaphoreCreateMutex();
  _spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // Mount synchronously: the transport asks right after boot whether it can
  // keep its store here. The first attempt after SPI init regularly fails
  // ("the physical drive cannot work") — the card needs a few milliseconds —
  // so retry before concluding there is no filesystem on it.
  _quietProbe = true;
  for (uint8_t attempt = 0; attempt < SD_MOUNT_ATTEMPTS && !_mounted; attempt++) {
    if (attempt) delay(SD_MOUNT_RETRY_MS);
    poll();
  }
  _quietProbe = false;
  if (!_mounted && info().state == State::Unformatted)
    log_w("SD card present (%.1f GB) but no recognised filesystem — format it from the settings page",
          info().cardBytes / 1e9);
  // 8 KB: the FAT layer (mount probes, rename on log rotation) left under
  // 1 KB of a 4 KB stack at idle and tripped the stack canary on core 0.
  xTaskCreatePinnedToCore(task, "sdcard", 8192, this, 1, nullptr, 0);
}

void SdCard::reserve(bool on) {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _reserved = on;
  xSemaphoreGive(_lock);
}

bool SdCard::reserved() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  bool r = _reserved;
  xSemaphoreGive(_lock);
  return r;
}

bool SdCard::storageLost() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  bool l = _storageLost;
  xSemaphoreGive(_lock);
  return l;
}

SdCard::Info SdCard::info() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  Info i = _info;
  xSemaphoreGive(_lock);
  return i;
}

bool SdCard::mounted() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  bool m = _mounted;
  xSemaphoreGive(_lock);
  return m;
}

bool SdCard::requestFormat() {
  if (_formatRequested) return false;
  // Formatting the card the Reticulum store is open on would pull the
  // filesystem out from under microStore mid-write.
  if (reserved()) { log_w("SD: format refused, the Reticulum store is on this card"); return false; }
  _formatRequested = true;
  return true;
}

void SdCard::task(void* self) {
  auto* sd = static_cast<SdCard*>(self);
  for (;;) {
    sd->poll();
    vTaskDelay(pdMS_TO_TICKS(SD_POLL_MS));
  }
}

// ---------------------------------------------------------------------------
// Card present but cannot be mounted? Talk to it below the filesystem.
// ---------------------------------------------------------------------------
bool SdCard::probeCardPresent() {
  uint8_t pdrv = sdcard_init(PIN_SD_CS, &_spi, SD_SPI_HZ);
  if (pdrv == 0xFF) return false;
  xSemaphoreTake(_lock, portMAX_DELAY);
  _info.type = sdcard_type(pdrv);
  _info.cardBytes = (uint64_t)sdcard_num_sectors(pdrv) * sdcard_sector_size(pdrv);
  xSemaphoreGive(_lock);
  sdcard_uninit(pdrv);
  return true;
}

bool SdCard::mount() {
  // sd_diskio's "f_mount failed" line on a miss comes from the Arduino HAL
  // logger, which has no runtime level control — it is expected during the
  // boot retries and on a card with a foreign filesystem.
  if (!SD.begin(PIN_SD_CS, _spi, SD_SPI_HZ, kMount, 8, false)) return false;
  _mounted = true;
  measure();
  SD.mkdir(kLogDir);
  File f = SD.open(kLogFile, FILE_APPEND);
  _logBytes = f ? f.size() : 0;
  if (f) f.close();
  return true;
}

void SdCard::unmount() {
  SD.end();
  _mounted = false;
}

void SdCard::measure() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _info.type        = SD.cardType();
  _info.cardBytes   = SD.cardSize();
  _info.volumeBytes = SD.totalBytes();
  _info.usedBytes   = SD.usedBytes();
  // A volume covering well under the card means foreign partitions
  // (Raspberry Pi boot+ext4, phone cards, ...) — offer a full format.
  bool partial = _info.cardBytes > 0 && _info.volumeBytes * 100 / _info.cardBytes < SD_PARTIAL_PERCENT;
  _info.state = partial ? State::Partial : State::Mounted;
  xSemaphoreGive(_lock);
}

void SdCard::poll() {
  if (_formatRequested) { doFormat(); _formatRequested = false; return; }

  if (_mounted) {
    // Removal check: a raw read of sector 0 fails once the card is gone.
    uint8_t sector[512];
    if (!SD.readRAW(sector, 0)) {
      bool res = reserved();
      if (res) log_e("SD card removed while the Reticulum store was on it — "
                     "path table and caches are frozen until the node reboots");
      else     log_w("SD card removed");
      unmount();
      xSemaphoreTake(_lock, portMAX_DELAY);
      _info = Info{};
      _reserved = res;                   // stays reserved: the store cannot move at runtime
      _storageLost = res;
      xSemaphoreGive(_lock);
    }
    return;
  }

  if (mount()) {
    Info i = info();
    log_i("SD card mounted: %s, card %.1f GB, volume %.1f GB (%s)",
          i.type == CARD_SDHC ? "SDHC" : i.type == CARD_SD ? "SD" : i.type == CARD_MMC ? "MMC" : "?",
          i.cardBytes / 1e9, i.volumeBytes / 1e9, stateName(i.state));
    log("boot: card mounted");
    return;
  }

  // No filesystem we understand — is there a card at all?
  bool present = probeCardPresent();
  xSemaphoreTake(_lock, portMAX_DELAY);
  State prev = _info.state;
  _info.state = present ? State::Unformatted : State::Absent;
  if (!present) { _info.type = CARD_NONE; _info.cardBytes = 0; }
  _info.volumeBytes = _info.usedBytes = 0;
  State now = _info.state;
  uint64_t bytes = _info.cardBytes;
  xSemaphoreGive(_lock);
  if (!_quietProbe && now != prev && now == State::Unformatted)
    log_w("SD card present (%.1f GB) but no recognised filesystem — format it from the settings page", bytes / 1e9);
}

// ---------------------------------------------------------------------------
// Full format: wipe the partition table, remount with format-on-empty so
// FatFS creates one FAT32 volume across the whole card. Destructive; only
// reachable through the authenticated settings API with confirmation.
// ---------------------------------------------------------------------------
void SdCard::doFormat() {
  xSemaphoreTake(_lock, portMAX_DELAY);
  _info.state = State::Formatting;
  strlcpy(_info.lastFormat, "in progress", sizeof(_info.lastFormat));
  xSemaphoreGive(_lock);
  log_w("SD: formatting card");

  if (_mounted) unmount();

  uint8_t pdrv = sdcard_init(PIN_SD_CS, &_spi, SD_SPI_HZ);
  if (pdrv == 0xFF) {
    xSemaphoreTake(_lock, portMAX_DELAY);
    _info.state = State::Absent;
    strlcpy(_info.lastFormat, "failed: no card", sizeof(_info.lastFormat));
    xSemaphoreGive(_lock);
    return;
  }
  uint8_t zeros[512] = {0};
  bool wiped = true;
  for (uint32_t s = 0; s < 8 && wiped; s++) wiped = sd_write_raw(pdrv, zeros, s);   // MBR + slack
  sdcard_uninit(pdrv);
  if (!wiped) {
    xSemaphoreTake(_lock, portMAX_DELAY);
    _info.state = State::Error;
    strlcpy(_info.lastFormat, "failed: write error", sizeof(_info.lastFormat));
    xSemaphoreGive(_lock);
    return;
  }

  uint32_t t0 = millis();
  bool ok = SD.begin(PIN_SD_CS, _spi, SD_SPI_HZ, kMount, 8, true);   // format_if_empty
  if (ok) {
    _mounted = true;
    measure();
    SD.mkdir(kLogDir);
    _logBytes = 0;
    Info i = info();
    xSemaphoreTake(_lock, portMAX_DELAY);
    snprintf(_info.lastFormat, sizeof(_info.lastFormat), "ok: %.1f GB in %lus", i.volumeBytes / 1e9, (unsigned long)((millis() - t0) / 1000));
    xSemaphoreGive(_lock);
    log_i("SD: format done, volume %.1f GB", i.volumeBytes / 1e9);
    log("boot: card formatted");
  } else {
    xSemaphoreTake(_lock, portMAX_DELAY);
    _info.state = State::Error;
    strlcpy(_info.lastFormat, "failed: mkfs", sizeof(_info.lastFormat));
    xSemaphoreGive(_lock);
    log_e("SD: format failed");
  }
}

// ---------------------------------------------------------------------------
void SdCard::log(const char* line) {
  if (!_mounted || !line) return;
  xSemaphoreTake(_lock, portMAX_DELAY);
  if (_logBytes > SD_LOG_MAX_BYTES) {
    SD.remove(SdCard::LOG_PREV_PATH);
    SD.rename(kLogFile, SdCard::LOG_PREV_PATH);
    _logBytes = 0;
  }
  File f = SD.open(kLogFile, FILE_APPEND);
  if (f) {
    size_t n = f.printf("%lu %s\n", (unsigned long)(millis() / 1000), line);
    f.close();
    _logBytes += n;
  }
  xSemaphoreGive(_lock);
}
