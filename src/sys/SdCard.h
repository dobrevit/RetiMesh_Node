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
#include <freertos/task.h>
#include "Config.h"
#include <atomic>

#if HAS_SD
#include <SD.h>
#endif
// A board with no slot does not build the card driver at all — the library,
// its FatFS layer and this class's real implementation stay out of the image,
// and the accessors below become the small always-false stubs in SdCard.cpp.
// Which is why the card-type fields below default to a literal 0 rather than
// to the library's CARD_NONE: the name is an enumerator the header may not
// have, and redeclaring it collides on any board where something else drags
// the library's headers in anyway. Zero is that enumerator's value.

class SdCard {
public:
  enum class State : uint8_t { Absent, Mounted, Partial, Unformatted, Formatting, Error };

  struct Info {
    State    state       = State::Absent;
    uint8_t  type        = 0;           // sdcard_type_t; 0 is CARD_NONE
    uint64_t cardBytes   = 0;           // raw capacity
    uint64_t volumeBytes = 0;           // mounted FAT volume
    uint64_t usedBytes   = 0;
    char     lastFormat[40] = "";       // result of the last format request
  };

  void begin();                          // mount, synchronously; no task yet
  void startPolling();                   // ... once the store's home is settled
  Info info();
  bool mounted();

  // Whether a state means "there is a filesystem mounted at /sd". Stated once
  // so that a caller holding an Info can answer it without a second mutex take
  // for mounted(), and without writing the pair of enumerators out again in a
  // place that would then have to be found when a state is added.
  static constexpr bool isMounted(State s) {
    return s == State::Mounted || s == State::Partial;
  }

  // "Somebody is at the node." Puts the poll cadence (SdPollPolicy) back on its
  // base beat and ends the current wait, so a card pushed in after the node has
  // been sitting with an empty slot is found at once rather than within the
  // policy's ceiling. Raise-only, safe from any task; the card task consumes it.
  //
  // Only unambiguous operator actions may call this. The button qualifies — a
  // hand on the node is the same hand that pushes a card in, and it is already
  // the wake for the idle access point. A status read does not: monitoring
  // tools and soak samplers poll /api/status for ever, and a poke from there
  // would hold the node on the three-second beat and silently undo the whole
  // back-off.
  //
  // "At once" is meant literally, which is why this is not just a flag: the
  // card task is woken out of its wait rather than left to notice the flag
  // when its current slice runs out. A press landing a millisecond after a
  // slice started used to wait very nearly three seconds for a look the button
  // and the documentation both promise immediately.
  void lookNow();

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
    uint8_t  type    = 0;               // sdcard_type_t; 0 is CARD_NONE
    uint64_t bytes   = 0;
  };

  // Why the card may not be formatted, or nullptr when it may be. The rule
  // lives here and nowhere else: the HTTP handler used to keep its own copy of
  // the "the store is on this card" refusal, and two statements of a rule are
  // two rules the moment one of them is edited.
  const char* formatRefusal();

  void  poll();                          // the slot, and the marker when it moves
  // Wakes the card task out of its wait. Raised by lookNow() and by
  // requestFormat(), after the flag they set, and a no-op before the task
  // exists.
  void  poke();
  // Waits the interval SdPollPolicy asked for, in watchdog-sized slices, and
  // ends the moment a format or a look is asked for — each slice is a task
  // notification with the slice as its timeout, so a request wakes the task
  // rather than waiting out the slice it landed in.
  void  wait(uint32_t ms);
  bool  checkSlot();                     // true when what the slot holds changed
  bool  mount();
  void  unmount();
  Probe probe();                         // low level: mount attempt + raw read
  void  doFormat();
  void  measure();

  // HSPI on every board that gives the slot wires of its own; the board says
  // otherwise where the card shares the radio's bus (Config.h, SD_SPI_BUS).
  // The host's bus rather than one of this driver's own — starting a second
  // SPIClass on a host the panel already started is what took two boards down.
  // See sys/SpiBus.h; set in begin().
  SPIClass*         _spi = nullptr;
  SemaphoreHandle_t _lock = nullptr;
  Info              _info;
  // Raised from any task, consumed on the card's own. Atomics rather than
  // volatile bools: volatile keeps the compiler from folding an access away
  // and promises nothing at all about what another task observes, which is
  // exactly what sharing a flag between tasks needs.
  //
  // The two of them that a poke follows are stored with release and read back
  // in wait() with acquire, which is stronger than the relaxed flags elsewhere
  // in this round and deliberately so: here the flag and the notification are
  // two separate objects that have to be seen in that order. A card task that
  // woke on the notification and then read the flag as not yet set would go
  // back to waiting with the wake already spent — and wait out the rest of an
  // interval that reaches half a minute, which is the delay this pairing
  // exists to remove. Everywhere else these are read for their own sake and
  // relaxed is the whole message.
  std::atomic<bool> _formatRequested{false};
  std::atomic<bool> _lookNow{false};      // an operator asked for a look (lookNow())
  // The card task, so a request can end its wait instead of waiting it out.
  // Recorded by the task itself at its first line, the way the GNSS reader
  // does (Gps.cpp); null until then, and a request raised in that window is
  // still served — the flags above are what the wait actually tests.
  std::atomic<TaskHandle_t> _task{nullptr};
  bool              _mounted = false;
  bool              _reserved = false;   // Reticulum store lives here
  bool              _storageLost = false;
  uint32_t          _logBytes = 0;
};

extern SdCard sdCard;
