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
//  LoRaFem.h — the amplified front end between the radio and the antenna
//
//  On every other board here the SX1262's antenna pin more or less reaches the
//  connector, and a radio that initialises is a radio that works. On the
//  Heltec V4 there is a front-end module in the way: a part that combines a
//  transmit amplifier and a receive LNA behind its own switch, on its own
//  power rail. Until that rail is up and the mode pins are driven, the
//  chip transmits into an open circuit and listens to one — while answering
//  every SPI register read perfectly. The failure this file prevents is a
//  node that reports itself online and is silent on the air.
//
//  Two parts, one footprint. Board revisions carry either a GC1109 or a
//  KCT8103L, wired so that most control lines land on the same GPIOs but the
//  meaning of the per-direction pin differs. Which part is fitted can be read
//  at boot: with the rail up, the shared CSD net reads high on a KCT8103L and
//  low on a GC1109. The detection is done once and the answer drives which
//  pin is flipped for which direction thereafter.
//
//      direction   GC1109                      KCT8103L
//      transmit    CPS (GPIO 46) high = PA     CTX (GPIO 5) high
//      receive     CPS low (LNA via CTX/DIO2)  CTX low = LNA in path
//
//  The radio's DIO2 drives the third control line on both parts, which is why
//  RF_DIO2_AS_SWITCH stays true on this board: the chip flips the part's own
//  TX/RX select in hardware, and this module only has to set the slower pins
//  that choose amplifier against bypass.
//
//  The amplifier's gain is not a constant. Both parts compress: roughly 11 dB
//  falling to 7 dB across the GC1109's drive range, 13 falling to 7 on the
//  KCT8103L. gainDb() carries the published curves so the radio can *report*
//  what is leaving the antenna — this firmware's convention is that the
//  configured power is what the chip is driven at and the operator accounts
//  for the amplifier, as on the SX1280+PA board.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_LORA_FEM

#include <stdint.h>

namespace LoRaFem {

enum class Part : uint8_t { None, GC1109, KCT8103L };

// Power the rail, work out which part is fitted, and leave it receiving.
// Called once from the radio's begin(), before the SX1262 is probed — the
// self-test that proves the radio proves the front end with it, and only if
// the front end is already up.
void begin();

// Point the part the right way. tx() before the chip transmits, rx() after it
// finishes — cheap pin writes, called around every frame.
void tx();
void rx();

const char* partName();

// The part's approximate transmit gain at a given chip drive, in dB, from the
// published curves. For reporting; nothing here changes the drive.
int8_t gainDb(int8_t chipDbm);

} // namespace LoRaFem

#else

// Boards without a front end compile the calls away rather than guarding
// every call site.
namespace LoRaFem {
inline void begin() {}
inline void tx() {}
inline void rx() {}
inline const char* partName() { return "none"; }
inline int8_t gainDb(int8_t) { return 0; }
} // namespace LoRaFem

#endif // HAS_LORA_FEM
