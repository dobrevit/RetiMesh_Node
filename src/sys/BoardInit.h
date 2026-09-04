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
//  BoardInit.h — what has to be true before any driver touches a bus
//
//  Two facts, both of which used to be nobody's job because no board here had
//  either:
//
//  The peripheral rail. Some boards put a load switch in front of everything —
//  radio, card, panel, both I2C residents — and until it is driven the board
//  looks broken in a way that reads exactly like a wiring fault: an empty I2C
//  bus and a transceiver that does not answer. It is not a PMU, so Pmu.h has
//  nothing to say about it; it is one GPIO, and it has to be the first one.
//
//  The idle chip selects. Where the panel, the radio and the card share one
//  SPI bus, the first driver to start talking does so with the other two chip
//  selects still floating — and a floating select is a device that may decide
//  it is being addressed. Every driver here raises its own select in its own
//  begin(), which is correct and too late: the first one to run is already on
//  a bus with two undefined listeners. So they are all idled here, once,
//  before any of them starts.
//
//  Both are no-ops on a board that declares neither, which is every board this
//  firmware ran on before the T-Deck.
// ============================================================================
#pragma once

namespace BoardInit {

// Raise the peripheral rail and idle every chip select on a shared bus.
// Called from setup() before LittleFS, the buses, the radio or the panel.
void begin();

} // namespace BoardInit
