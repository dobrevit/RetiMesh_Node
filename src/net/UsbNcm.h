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
//  UsbNcm.h — the S3's composite USB device and the network link behind it
//
//  On a board whose own USB reaches the connector (HAS_USB_NCM), the OTG
//  stack owns the peripheral and presents one device with two functions:
//
//    CDC-ACM  the console and the log (Serial); the core's function, and it
//             names the interface itself
//    CDC-NCM  "RetiMesh Network"      an Ethernet-over-USB link, usb0
//
//  The ACM function is the core's: with ARDUINO_USB_CDC_ON_BOOT the core
//  enables it and starts the device before setup() runs. The NCM function
//  is this file's, and because the device's descriptors are assembled once,
//  at that start, the function is registered from a static initialiser —
//  the one moment that is guaranteed to come first.
//
//  Behind the NCM function sits an esp_netif on the default Ethernet stack
//  with a DHCP server, at usbNodeAddress(<last MAC octet>) (LocalLinkState.h).
//  The services already bind 0.0.0.0, so HTTP, the transport and mDNS reach
//  the host over it with nothing more to do. The link is brought up once the
//  host has enumerated the device (the class driver does not report the data
//  interface being opened; the first frame decides the rest) and down when
//  the cable goes or the operator switches usb off; the device itself stays
//  enumerated either way.
//
//  Every transition of the network interface runs from poll(), on the loop
//  task: the TinyUSB callbacks arrive on the USB task and only record what
//  happened, because esp_netif's actions block on the TCP/IP task and must
//  not be called from a driver callback.
//
//  What the switch costs. links.usb does not merely decide whether usb0
//  answers: with it off the interface, its DHCP server and the six-kilobyte
//  transmit ring do not exist. Measured on a T3-S3, switching it off and on:
//  6748 B of byte-addressable internal RAM, four cycles running with the
//  figure returning to the same value each time. Before that the ring was
//  .bss and the interface was built in begin(), so the switch was worth 68
//  bytes — the switches existed and meant nothing to the allocator, which is
//  the whole of the backlog row this closes.
//
//  What it does not give back is the composite device itself. The
//  descriptors are assembled once, before setup() runs, and the host keeps
//  its ACM port either way; only the network function behind it comes and
//  goes, which is what tud_network_link_state tells the host.
//
//  Switching off is stepped across passes of the loop and never blocks it,
//  because nothing here may be freed while the USB task or the TCP/IP task
//  can still be inside it: the link goes down, esp_netif is stopped (which
//  is itself the TCP/IP task's barrier), the retry timer is stopped, a no-op
//  is run on the USB task to flush what was queued there, and only then are
//  the interface and the ring released. If that last step never answers, the
//  memory stays claimed — not freeing is harmless, freeing under a live
//  callback is not.
// ============================================================================
#pragma once

#include <stdint.h>
#include "Config.h"

#if HAS_USB_NCM
#include <IPAddress.h>

namespace UsbNcm {

// What the device needs whether or not the network link is switched on: the
// identity the descriptors are built from, the 1200-baud touch, and the
// transmit retry timer. Nothing that the switch decides. Once, after the
// network stack exists (WifiManager::begin() brings it up whether or not
// Wi-Fi is on).
void begin();

// Applies the switch and any USB event recorded since the last pass, and
// builds or gives back what the switch decides (above). From the loop task
// only, every pass — a teardown takes several of them.
void poll(bool enabled);

// Takes the device off the bus — the network interface down, the
// controller disconnected, the wire pulled to a single-ended zero — before
// the restart into the ROM downloader, so that the host has every chance
// to see one device go before the serial-JTAG unit appears on the same
// pins. The core's own hand-over swaps the two pull-ups within
// microseconds. See detach() for what a hub makes of that.
void detach();

// Whether the interface is up: the device is mounted and the switch is on.
bool linkUp();

// Whether the host has opened its side (set a packet filter) since the
// device was last mounted. Diagnostic; the link does not wait for it.
bool hostOpened();

// The node's address on the link.
IPAddress address();

// Packets moved since boot, for the diagnostics that want them.
uint32_t rxPackets();
uint32_t txPackets();
uint32_t txDropped();

} // namespace UsbNcm
#endif
