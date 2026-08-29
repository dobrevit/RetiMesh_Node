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
//    CDC-ACM  "RetiMesh Maintenance"  the console and the log (Serial)
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
//  the host over it with nothing more to do. The link is brought up when the
//  host activates the data interface and down when the cable goes or the
//  operator switches usb off; the device itself stays enumerated either way.
//
//  Every transition of the network interface runs from poll(), on the loop
//  task: the TinyUSB callbacks arrive on the USB task and only record what
//  happened, because esp_netif's actions block on the TCP/IP task and must
//  not be called from a driver callback.
// ============================================================================
#pragma once

#include <stdint.h>
#include "Config.h"

#if HAS_USB_NCM
#include <IPAddress.h>

namespace UsbNcm {

// Creates the network interface. Once, after the network stack exists
// (WifiManager::begin() brings it up whether or not Wi-Fi is on).
void begin();

// Applies the switch and any USB event recorded since the last pass. From
// the loop task only.
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
