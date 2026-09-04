// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  SpiBus.cpp — see SpiBus.h
// ============================================================================
#include "SpiBus.h"

#include <Arduino.h>

namespace {

// One slot per host the chip has. FSPI and HSPI are 0 and 1 on the S3-class
// parts and 1..3 on the classic ESP32, so the table is sized for the widest
// numbering rather than the narrowest.
constexpr uint8_t kMaxHosts = 4;

struct Slot {
  SPIClass* bus = nullptr;
  bool      started = false;
  int8_t    sck = -1, miso = -1, mosi = -1;   // what the first caller started it on
};

Slot sSlots[kMaxHosts];

} // namespace

namespace SpiBus {

SPIClass& get(uint8_t host, int8_t sck, int8_t miso, int8_t mosi) {
  // A host number the chip does not have is a board-header mistake, and
  // returning some other host's bus would hide it behind a device that
  // silently never answers. Clamp loudly instead.
  if (host >= kMaxHosts) {
    log_e("SPI host %u does not exist — check the board header; using host 0", (unsigned)host);
    host = 0;
  }
  Slot& s = sSlots[host];
  if (!s.bus) s.bus = new SPIClass(host);
  if (!s.started) {
    // No chip select: SPIClass only records it, and every driver here drives
    // its own by hand. Passing one would suggest the peripheral does it.
    s.bus->begin(sck, miso, mosi, -1);
    s.sck = sck; s.miso = miso; s.mosi = mosi;
    s.started = true;
    log_i("SPI host %u up (SCK %d, MISO %d, MOSI %d)", (unsigned)host, sck, miso, mosi);
  } else if (s.sck != sck || s.miso != miso || s.mosi != mosi) {
    // Two devices on one host are on one set of wires by definition, so a
    // second caller naming different pins is a board header that disagrees with
    // itself. The first caller's wiring is what the peripheral is on and what
    // this returns; saying nothing would leave the second device silent for a
    // reason that reads exactly like a wiring fault — which is the failure this
    // whole file exists to stop.
    log_e("SPI host %u was started on SCK %d, MISO %d, MOSI %d and is now asked for %d, %d, %d "
          "— check the board header; the first wiring stands",
          (unsigned)host, s.sck, s.miso, s.mosi, sck, miso, mosi);
  }
  return *s.bus;
}

} // namespace SpiBus
