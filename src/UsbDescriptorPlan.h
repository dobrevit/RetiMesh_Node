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
//  UsbDescriptorPlan.h — the composite USB device, and whether it fits
//
//  The native-USB boards are meant to present one device with two functions
//  on one cable: CDC-NCM for IP, CDC-ACM for the maintenance console. Every
//  function costs endpoints, and the ESP32-S3's USB OTG controller has fewer
//  than the descriptor macros let you ask for. TinyUSB's port for it
//  (portable/espressif/esp32sx/dcd_esp32sx.c) says so in two constants:
//
//      EP_MAX      7    endpoints 0..6, each direction
//      EP_FIFO_NUM 5    transmit FIFOs, and FIFO0 belongs to EP0
//
//  so an application gets four IN endpoints (1..4) and six OUT endpoints. A
//  CDC function needs two IN (notification + data) and one OUT. Two of them
//  need four IN — exactly what there is, with none to spare. That is worth
//  knowing before a third function is added, which is what the static
//  assertions below are for: a descriptor that cannot enumerate should fail
//  to compile rather than fail on the bench.
//
//  Identity. The strings are fixed here. The VID:PID is the part that is not
//  ours to choose: Espressif allocates PIDs under its 0x303A to open-source
//  projects on request (github.com/espressif/usb-pids), and until one has
//  been granted the pair below is pid.codes' test allocation, which their
//  policy permits for development and forbids for anything shipped. Host
//  tooling therefore identifies a RetiMesh device by the product string and
//  the console's VERSION reply, never by the PID alone.
//
//  Nothing in here talks to the USB peripheral. The composite device itself
//  waits on a toolchain whose TinyUSB carries the NCM class — see
//  docs/local-link.md — and this file is the part of that work which is true
//  regardless.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace UsbPlan {

struct Function {
  const char* name;
  uint8_t inEndpoints;      // IN endpoints (device -> host), excluding EP0
  uint8_t outEndpoints;     // OUT endpoints (host -> device), excluding EP0
  uint8_t interfaces;       // interface descriptors the function occupies
};

// CDC class functions: one notification IN, one bulk IN, one bulk OUT, and a
// communication + data interface pair each.
constexpr Function CDC_NCM { "RetiMesh Network",     2, 1, 2 };
constexpr Function CDC_ACM { "RetiMesh Maintenance", 2, 1, 2 };

struct Budget {
  uint8_t inEndpoints;
  uint8_t outEndpoints;
  uint8_t interfaces;       // a practical cap; the descriptor field is 8 bits
};

// From dcd_esp32sx.c as shipped with the Arduino core's TinyUSB and with
// ESP-IDF's: EP_FIFO_NUM 5 (FIFO0 = EP0) and EP_MAX 7 (EP0..EP6).
constexpr Budget ESP32S3 { 4, 6, 8 };

struct Usage {
  uint8_t inEndpoints = 0;
  uint8_t outEndpoints = 0;
  uint8_t interfaces = 0;
};

constexpr Usage usage(const Function* fns, size_t n) {
  Usage u;
  for (size_t i = 0; i < n; i++) {
    u.inEndpoints  = (uint8_t)(u.inEndpoints  + fns[i].inEndpoints);
    u.outEndpoints = (uint8_t)(u.outEndpoints + fns[i].outEndpoints);
    u.interfaces   = (uint8_t)(u.interfaces   + fns[i].interfaces);
  }
  return u;
}

constexpr bool fits(const Function* fns, size_t n, const Budget& b) {
  const Usage u = usage(fns, n);
  return u.inEndpoints <= b.inEndpoints && u.outEndpoints <= b.outEndpoints &&
         u.interfaces <= b.interfaces;
}

// The RetiMesh composite: network first so it takes interface 0/1 and gets
// the lowest endpoint numbers, maintenance after it.
constexpr Function COMPOSITE[] = { CDC_NCM, CDC_ACM };
constexpr size_t   COMPOSITE_COUNT = sizeof(COMPOSITE) / sizeof(COMPOSITE[0]);

static_assert(fits(COMPOSITE, COMPOSITE_COUNT, ESP32S3),
              "the RetiMesh composite USB device does not fit the ESP32-S3's endpoints");
static_assert(usage(COMPOSITE, COMPOSITE_COUNT).inEndpoints == ESP32S3.inEndpoints,
              "the composite is expected to use every IN endpoint the S3 has; "
              "if that changed, re-check dcd_esp32sx.c before adding a function");

// Identity strings. The serial number is per device (the factory MAC) and is
// filled in at runtime; it is not a constant.
constexpr const char* MANUFACTURER = "RetiMesh";
constexpr const char* PRODUCT      = "RetiMesh Node";
constexpr uint16_t VID = 0x1209;   // pid.codes, test allocation — see the header comment
constexpr uint16_t PID = 0x0001;   // MUST be replaced by an allocated PID before release
constexpr bool     PID_IS_TEST_ALLOCATION = true;

// What the S3 enumerates as today, in its fixed USB-Serial/JTAG personality,
// both when the application runs and when the ROM downloader does. Host
// tooling looks for this pair; the composite device above will be a second.
constexpr uint16_t ESPRESSIF_VID        = 0x303A;
constexpr uint16_t ESP_USB_SERIAL_JTAG_PID = 0x1001;

} // namespace UsbPlan
