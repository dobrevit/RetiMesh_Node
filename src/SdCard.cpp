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
#include "StoreHome.h"

SdCard sdCard;

static const char* kMount = SdCard::MOUNT_POINT;
// Only ever mounted for the presence probe, and unmounted immediately.
static const char* kProbeMount = "/sdprobe";
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
  // keep its store here, and that decision cannot be revisited later. The
  // first attempt after SPI init regularly fails ("the physical drive cannot
  // work") because the card is still waking up, so retry — but with plain
  // mounts only. Going through poll() also ran the low-level presence probe
  // between attempts, which re-initialises the card and left it unready for
  // the next mount: the card would then turn up seconds later on the poll
  // task, after the store had already fallen back to internal flash.
  for (uint8_t attempt = 0; attempt < SD_MOUNT_ATTEMPTS && !_mounted; attempt++) {
    if (attempt) delay(SD_MOUNT_RETRY_MS);
    if (mount()) {
      Info i = info();
      log_i("SD card mounted: %s, card %.1f GB, volume %.1f GB (%s)%s",
            i.type == CARD_SDHC ? "SDHC" : i.type == CARD_SD ? "SD" : i.type == CARD_MMC ? "MMC" : "?",
            i.cardBytes / 1e9, i.volumeBytes / 1e9, stateName(i.state),
            attempt ? " [after retry]" : "");
      log("boot: card mounted");
    }
  }
  if (!_mounted) checkSlot();            // settles the state: unformatted, or no card
}

// Separate from begin() and called after the store has been moved and its home
// chosen. Between those two points the boot task is copying files across this
// same bus, and a poll that ran alongside it could answer a read it had lost to
// the copy by concluding the card was gone and unmounting it — freeing the card
// struct under the copy. The task not existing yet is what rules that out;
// a flag checked at the top of poll() only ever stopped the poll after the one
// that mattered.
void SdCard::startPolling() {
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
  if (!_lock) return false;

  xSemaphoreTake(_lock, portMAX_DELAY);
  bool r = _reserved;
  xSemaphoreGive(_lock);
  return r;
}

bool SdCard::storageLost() {
  if (!_lock) return false;

  xSemaphoreTake(_lock, portMAX_DELAY);
  bool l = _storageLost;
  xSemaphoreGive(_lock);
  return l;
}

SdCard::Info SdCard::info() {
  if (!_lock) return Info{};

  xSemaphoreTake(_lock, portMAX_DELAY);
  Info i = _info;
  xSemaphoreGive(_lock);
  return i;
}

bool SdCard::mounted() {
  if (!_lock) return false;

  xSemaphoreTake(_lock, portMAX_DELAY);
  bool m = _mounted;
  xSemaphoreGive(_lock);
  return m;
}

const char* SdCard::formatRefusal() {
  if (_formatRequested) return "a format is already running";
  if (info().state == State::Absent) return "no card";
  // Formatting the card the Reticulum store is open on would pull the
  // filesystem out from under microStore mid-write, and formatting one a
  // queued move is about to copy the store onto would erase it immediately
  // afterwards. StoreHome is asked rather than second-guessed: it is the only
  // thing that knows a move has been asked for and not made yet.
  if (StoreHome::busy()) return "the store is being moved; let the node restart first";
  if (reserved()) return "the Reticulum store is on this card; eject it first";
  return nullptr;
}

const char* SdCard::requestFormat() {
  const char* why = formatRefusal();
  if (why) { log_w("SD: format refused, %s", why); return why; }
  _formatRequested = true;
  return nullptr;
}

void SdCard::task(void* self) {
  auto* sd = static_cast<SdCard*>(self);
  for (;;) {
    sd->poll();
    vTaskDelay(pdMS_TO_TICKS(SD_POLL_MS));
  }
}

// ---------------------------------------------------------------------------
// One conversation with the card, not two
//
// "Does it mount" and "is anything in the slot" are the same question asked at
// two levels, and the expensive part belongs to both: the disk-layer
// initialise, which spends half a second on an empty slot because that is how
// long the driver waits for an answer before giving up. A mount attempt
// followed by a separate probe spent that half second twice, every three
// seconds, for as long as the slot stayed empty.
//
// SD.begin() cannot be asked the second question. When the FAT mount fails it
// frees the driver slot, and with it the card type and the sector count the
// initialise had just filled in — the very facts a probe then has to go back
// and ask the card for all over again. Driving the two steps by hand keeps the
// slot open long enough to read them off, and a card that does turn out to
// carry a filesystem is handed to SDFS afterwards, since owning the mount is
// the one thing this cannot do for it.
// ---------------------------------------------------------------------------
SdCard::Probe SdCard::probe() {
  Probe p;
  uint8_t pdrv = sdcard_init(PIN_SD_CS, &_spi, SD_SPI_HZ);
  if (pdrv == 0xFF) return p;

  // What sdcard_init actually does is take a free driver slot and allocate a
  // struct for it. It does not speak to the card, and the struct is malloc'd
  // rather than zeroed, so its sector count is whatever the heap last held
  // there. Reporting a card on the strength of getting a slot, and its size
  // from that field, is how an empty slot came to announce a card of some
  // arbitrary capacity — a terabyte, on occasion — and then sit there saying
  // it could not be mounted.
  //
  // The sector count is filled in by the disk initialise, and a mount attempt
  // is the only exported call that reaches it. Whether that mount succeeds is
  // an answer worth keeping, but it is the raw read of sector zero that
  // separates a card with no filesystem from no card at all.
  //
  // A mount point of its own, because the real one may still be registered from
  // an attempt that failed, and re-registering a path returns an error before
  // the initialise is ever reached. Releasing it again is sdcard_uninit()'s
  // job — it unregisters the path the mount recorded, and a mount that failed
  // has already unregistered its own — so there is nothing to undo here.
  p.fat = sdcard_mount(pdrv, kProbeMount, 1, false);
  uint8_t sector[512];
  p.present = p.fat || sd_read_raw(pdrv, sector, 0);
  if (p.present) {
    p.type  = sdcard_type(pdrv);
    p.bytes = (uint64_t)sdcard_num_sectors(pdrv) * sdcard_sector_size(pdrv);
  }
  if (p.fat) sdcard_unmount(pdrv);
  sdcard_uninit(pdrv);
  return p;
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
  // The store's ownership marker is read here, on the task that owns the card,
  // and handed to the status API from memory: it used to be opened and parsed
  // on the web server's task, more than once per request, on a bus this task
  // was using at the same moment. Only when the slot has changed under it,
  // though — a card that has come or gone, or a format that has just rewritten
  // everything on it. Re-reading it on every poll was an open, a JSON parse and
  // a directory probe every three seconds for the life of the node, to catch a
  // file that nothing but this node's own actions can change.
  if (checkSlot()) StoreHome::refreshCache();
}

bool SdCard::checkSlot() {
  if (_formatRequested) { doFormat(); _formatRequested = false; return true; }

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
      return true;
    }
    return false;
  }

  // Nothing mounted: one attempt, and it answers both questions. Only a card
  // that has just shown it carries a filesystem is worth handing to SDFS,
  // which does its own initialise — so the empty slot, which is the case that
  // repeats for hours, costs one initialise per poll instead of two.
  const Probe p = probe();
  if (p.fat && mount()) {
    Info i = info();
    log_i("SD card mounted: %s, card %.1f GB, volume %.1f GB (%s)",
          i.type == CARD_SDHC ? "SDHC" : i.type == CARD_SD ? "SD" : i.type == CARD_MMC ? "MMC" : "?",
          i.cardBytes / 1e9, i.volumeBytes / 1e9, stateName(i.state));
    log("boot: card mounted");
    return true;
  }

  xSemaphoreTake(_lock, portMAX_DELAY);
  State prev = _info.state;
  // A card that mounted a moment ago and then would not mount again is not an
  // unformatted card; it is one the next poll should try again, and calling it
  // unformatted invites an operator to format a card that has a filesystem.
  _info.state     = !p.present ? State::Absent : p.fat ? State::Error : State::Unformatted;
  _info.type      = p.present ? p.type  : CARD_NONE;
  _info.cardBytes = p.present ? p.bytes : 0;
  _info.volumeBytes = _info.usedBytes = 0;
  State now = _info.state;
  uint64_t bytes = _info.cardBytes;
  xSemaphoreGive(_lock);
  if (now != prev && now == State::Unformatted)
    log_w("SD card present (%.1f GB) but no recognised filesystem — format it from the settings page", bytes / 1e9);
  // Nothing mounted before and nothing mounted now: whatever is in the slot,
  // there is no filesystem to read a marker off.
  return false;
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
  // The card has to be woken before it will take a raw write. sdcard_init()
  // only registers the disk driver and leaves the card flagged not-initialised;
  // the flag is cleared by the disk layer's own initialise, which nothing but a
  // mount attempt reaches. Without this every sd_write_raw() below returns
  // "not ready" on the first sector, the wipe gives up, and mkfs is never
  // reached — so formatting failed identically on every card, which is exactly
  // what it did until this line existed.
  //
  // Whether the mount succeeds is beside the point and deliberately ignored: an
  // unformatted card is the usual reason to be here and will fail to mount, but
  // it is initialised either way, which is all the wipe needs.
  const bool mounted = sdcard_mount(pdrv, kMount, 1, false);

  uint8_t zeros[512] = {0};
  bool wiped = true;
  for (uint32_t s = 0; s < 8 && wiped; s++) wiped = sd_write_raw(pdrv, zeros, s);   // MBR + slack
  // The VFS path comes free with the driver: sdcard_uninit() unregisters
  // whatever the mount recorded, which is what lets the SD.begin() below
  // register the same name again.
  if (mounted) sdcard_unmount(pdrv);
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
  if (!_lock || !_mounted || !line) return;
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
