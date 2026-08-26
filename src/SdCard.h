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
//  SdCard.h — microSD slot: hot-plug, capacity check, format, event log
//
//  The card is optional; nothing depends on it. A low-priority task polls
//  the slot: when a card appears it is mounted (FAT) at /sd and measured —
//  a mounted volume much smaller than the card (e.g. a Raspberry Pi image:
//  small FAT32 boot partition + ext4) is reported as "partial", a card with
//  no recognised filesystem as "unformatted". Both can be formatted to a
//  single FAT32 volume from the admin page (zero the MBR, remount with
//  format-on-empty, which lets FatFS mkfs the whole card).
//
//  Runs on its own SPI bus (HSPI) — it never contends with the radio.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Config.h"

class SdCard {
public:
  enum class State : uint8_t { Absent, Mounted, Partial, Unformatted, Formatting, Error };

  struct Info {
    State    state       = State::Absent;
    uint8_t  type        = CARD_NONE;   // sdcard_type_t
    uint64_t cardBytes   = 0;           // raw capacity
    uint64_t volumeBytes = 0;           // mounted FAT volume
    uint64_t usedBytes   = 0;
    char     lastFormat[40] = "";       // result of the last format request
  };

  void begin();                          // creates the poll task
  Info info();
  bool mounted();
  bool requestFormat();                  // performed by the task; false if busy

  static constexpr const char* LOG_PATH = "/retimesh/events.log";        // relative to /sd
  static constexpr const char* LOG_PREV_PATH = "/retimesh/events.1.log";

  // Appends a line to /sd/retimesh/events.log (rotated at SD_LOG_MAX_BYTES).
  // Cheap no-op when no card is mounted.
  void log(const char* line);

  static const char* stateName(State s);
  static void task(void* self);

private:
  void poll();
  bool mount();
  void unmount();
  bool probeCardPresent();               // low level, without mounting
  void doFormat();
  void measure();

  SPIClass          _spi{HSPI};
  SemaphoreHandle_t _lock = nullptr;
  Info              _info;
  volatile bool     _formatRequested = false;
  bool              _mounted = false;
  uint32_t          _logBytes = 0;
};

extern SdCard sdCard;
