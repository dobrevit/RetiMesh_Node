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
//  SpiBus.h — one owner per SPI host
//
//  Three drivers here talk SPI: the panel, the transceiver and the card. Each
//  used to construct an SPIClass of its own and call begin() on it, which was
//  correct for as long as every one of them had a host to itself — and every
//  board did, until two of them arrived with all three devices on one set of
//  wires.
//
//  Two SPIClass objects on the same host are not two buses. Arduino's
//  SPIClass::begin() guards against being called twice on *itself*, not
//  against another object starting the same peripheral, so the second driver's
//  begin() re-runs the whole bus setup underneath the first: it re-attaches the
//  pins, re-registers the APB-change callback the core refuses as a duplicate,
//  and reconfigures a bus that another driver believes it already owns. On the
//  bench that took two boards down at exactly the same line — the card's
//  begin(), moments after the panel's.
//
//  So a host is fetched rather than constructed. The first caller's pins start
//  it; everyone after gets the same object, and therefore the same underlying
//  bus lock that beginTransaction takes. Boards where the devices genuinely sit
//  on different hosts are unaffected: they ask for different hosts and get
//  different objects, which is what they had before.
//
//  Sharing the object is what makes sharing the wires safe. The mutual
//  exclusion was always in the core — every driver here brackets its transfers
//  with beginTransaction/endTransaction, and none attaches a hardware chip
//  select — but it only applies to drivers that agree on which bus they are on.
// ============================================================================
#pragma once

#include <SPI.h>

namespace SpiBus {

// The SPIClass for `host`, started on the first call with the pins given.
// Later callers get the same object and their pins are ignored — a board whose
// devices share a host must give the same wiring, which is what sharing means.
// Chip selects are never passed here: every driver drives its own by hand.
SPIClass& get(uint8_t host, int8_t sck, int8_t miso, int8_t mosi);

} // namespace SpiBus
