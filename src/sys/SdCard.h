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
//
//  Boards without a slot (HAS_SD 0) never call begin(), so every accessor
//  answers "no card" rather than touching a mutex that was never created.
//
//  Mounting and polling are two calls, in that order, with the store's move
//  between them. begin() mounts synchronously so that whoever boots next — the
//  migration, and then the choice of where the store lives — sees the final
//  state; startPolling() is called once those are done. They drive this same
//  card for seconds at a time, on the same bus, and the removal check answers a
//  read that loses the bus with unmount(), which frees the card struct
//  underneath them. Not starting the task until they have finished is the only
//  thing that actually prevents that: a flag tested at the top of poll() stops
//  the next poll, not the one already inside the check.
//
//  The poll also re-reads the store's ownership marker (StoreHome) when the
//  slot changes, because this is the task that owns the card: everyone else is
//  handed the answer from memory rather than opening files on this bus behind
//  its back.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Config.h"

#if HAS_SD
#include <SD.h>
#else
// A board with no slot does not build the card driver at all — the library, its
// FatFS layer and this class's real implementation stay out of the image, and
// the accessors below become the small always-false stubs in SdCard.cpp. Only
// one of the library's names leaks into this header: the card type that means
// "nothing", which is zero in the driver's own enum.
static const uint8_t CARD_NONE = 0;
#endif

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

  void begin();                          // mount, synchronously; no task yet
  void startPolling();                   // ... once the store's home is settled
  Info info();
  bool mounted();

  // Asks for a format, and answers with the reason it was refused or nullptr
  // when it was accepted. One call, because the rule turns on a card and a
  // queued move and both can change between two readings of it: this used to
  // answer true or false and leave the caller to ask formatRefusal() a second
  // time for something to say, which could name a reason that no longer applied
  // or come back with nothing at all for a request that had just been refused.
  // Which is why that rule is now private — there is one way to ask.
  const char* requestFormat();           // performed by the task

  // "The Reticulum store lives on this card." Set at boot by RnsTransport
  // when it puts its microStore files on the card. While reserved the card
  // may not be formatted, and losing it is an error rather than a shrug.
  void reserve(bool on);
  bool reserved();
  bool storageLost();                    // reserved card was removed

  // Where the volume is mounted in the VFS. Named here because StoreHome builds
  // paths for the silent stat() it uses to look at the card without opening
  // anything, and a second spelling of "/sd" is a second thing to keep in step.
  static constexpr const char* MOUNT_POINT = "/sd";

  static constexpr const char* LOG_PATH = "/retimesh/events.log";        // relative to /sd
  static constexpr const char* LOG_PREV_PATH = "/retimesh/events.1.log";

  // Appends a line to /sd/retimesh/events.log (rotated at SD_LOG_MAX_BYTES).
  // Cheap no-op when no card is mounted.
  void log(const char* line);

  static const char* stateName(State s);
  static void task(void* self);

private:
  // What one attempt at the card says: whether anything is in the slot, and
  // whether it carries a filesystem this node can mount. Both answers come out
  // of the same disk-layer initialise, which is the only part that costs
  // anything (see probe()).
  struct Probe {
    bool     present = false;
    bool     fat     = false;
    uint8_t  type    = CARD_NONE;
    uint64_t bytes   = 0;
  };

  // Why the card may not be formatted, or nullptr when it may be. The rule
  // lives here and nowhere else: the HTTP handler used to keep its own copy of
  // the "the store is on this card" refusal, and two statements of a rule are
  // two rules the moment one of them is edited.
  const char* formatRefusal();

  void  poll();                          // the slot, and the marker when it moves
  bool  checkSlot();                     // true when what the slot holds changed
  bool  mount();
  void  unmount();
  Probe probe();                         // low level: mount attempt + raw read
  void  doFormat();
  void  measure();

  SPIClass          _spi{HSPI};
  SemaphoreHandle_t _lock = nullptr;
  Info              _info;
  volatile bool     _formatRequested = false;
  bool              _mounted = false;
  bool              _reserved = false;   // Reticulum store lives here
  bool              _storageLost = false;
  uint32_t          _logBytes = 0;
};

extern SdCard sdCard;
