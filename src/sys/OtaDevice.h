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
//  OtaDevice.h — the parts of an update that only exist on the hardware
//
//  Deliberately thin. Everything with a decision in it lives in OtaInstaller.h
//  and OtaFloor.h where a host can run it; what is left here is esp_ota calls,
//  NVS keys, and the one thing neither of those can know — whether this image
//  is the one that was staged.
// ============================================================================
#pragma once

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "OtaFloor.h"
#include "OtaInstaller.h"

namespace Ota {

// The floor and the staged record, in their own NVS namespace so nothing here
// can collide with a settings key.
class NvsStore {
 public:
  void begin() { if (!_open) _open = _prefs.begin("ota", false); }
  bool has(const char* key) { return _prefs.isKey(key); }
  uint32_t get(const char* key, uint32_t fallback) { return _prefs.getUInt(key, fallback); }
  void put(const char* key, uint32_t value) { _prefs.putUInt(key, value); }
  void drop(const char* key) { _prefs.remove(key); }

 private:
  Preferences _prefs;
  bool _open = false;
};

// The slot an update is written to, resolved once: an install must not be able
// to write one partition and switch to another because something re-answered
// the question halfway through.
class EspTarget {
 public:
  EspTarget() : _slot(esp_ota_get_next_update_partition(nullptr)) {}

  bool haveSlot() const { return _slot != nullptr; }
  uint32_t slotSize() const { return _slot ? _slot->size : 0; }
  uint32_t slotId() const { return _slot ? _slot->address : 0; }

  bool begin(size_t imageSize) {
    return _slot && esp_ota_begin(_slot, imageSize, &_handle) == ESP_OK;
  }
  bool write(const uint8_t* data, size_t len) {
    return esp_ota_write(_handle, data, len) == ESP_OK;
  }
  bool finish() {
    const bool ok = esp_ota_end(_handle) == ESP_OK;
    _handle = 0;
    return ok;
  }
  // Straight off the partition rather than through any cache the write path
  // may have kept, because the question is what is in flash.
  bool readBack(size_t offset, uint8_t* buf, size_t len) {
    return _slot && esp_partition_read(_slot, offset, buf, len) == ESP_OK;
  }
  bool switchTo() { return _slot && esp_ota_set_boot_partition(_slot) == ESP_OK; }

 private:
  const esp_partition_t* _slot;
  esp_ota_handle_t       _handle = 0;
};

using DeviceInstaller = Installer<EspTarget, NvsStore>;

// Called once this image has got far enough to be worth keeping.
//
// Two things happen here because they are the same statement — "this image
// runs". The bootloader starts an updated app in PENDING_VERIFY and puts the
// old one back on the next boot unless the new one says otherwise, so not
// calling this is what makes a bad update recoverable; calling it too early is
// what would throw that away.
//
// The bar it actually clears is "reached the end of start-up without a panic",
// which is where a bad image usually fails. It is not a promise that the radio
// works, and a node that dies an hour in has already been kept.
inline Settlement confirmBoot(Floor<NvsStore>& floor) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }
  return floor.confirm(running ? running->address : 0);
}

// Whether this build could install an update at all: a board whose table has a
// single app partition cannot, and says so once at boot rather than at the
// moment someone is trying to update it.
inline bool canSelfUpdate() { return esp_ota_get_next_update_partition(nullptr) != nullptr; }

}  // namespace Ota
