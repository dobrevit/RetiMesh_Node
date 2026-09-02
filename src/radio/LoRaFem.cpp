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
//  LoRaFem.cpp — see LoRaFem.h
// ============================================================================
#include "LoRaFem.h"

#if HAS_LORA_FEM

#include <Arduino.h>

namespace {

LoRaFem::Part sPart = LoRaFem::Part::None;

// The published transmit gain of each part against chip drive, 0..21 dBm.
// Indexed by drive; both curves flatten at the low end and compress at the
// top. Values are the vendor's measurements of this board's RF path — net
// gain including the attenuator in front of the amplifier, not the bare
// part's figure.
constexpr int8_t kGc1109Gain[22] = { 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
                                     11, 11, 11, 11, 11, 10, 10, 9, 9, 8, 7 };
constexpr int8_t kKctGain[22]    = { 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
                                     13, 13, 13, 12, 12, 11, 11, 10, 9, 8, 7 };

// The pin that chooses amplifier against bypass for the part that is fitted.
// The other line of the pair is driven by the radio's DIO2 in hardware.
int modePin() {
  return sPart == LoRaFem::Part::KCT8103L ? PIN_FEM_MODE_KCT : PIN_FEM_MODE_GC1109;
}

} // namespace

namespace LoRaFem {

void begin() {
  // The rail first: the detection below reads a pin on the part, and a part
  // with no power answers nothing.
  pinMode(PIN_FEM_POWER, OUTPUT);
  digitalWrite(PIN_FEM_POWER, HIGH);
  delay(5);

  // Which part is fitted. The shared enable net reads high on a KCT8103L and
  // low on a GC1109 while it is an input — the parts bias it differently, and
  // this is the one moment the difference is visible from here.
  pinMode(PIN_FEM_ENABLE, INPUT);
  delay(1);
  sPart = digitalRead(PIN_FEM_ENABLE) == HIGH ? Part::KCT8103L : Part::GC1109;

  // Enabled, and receiving: low on the mode pin is the LNA path on both
  // parts, and listening is the state the radio spends its life in.
  pinMode(PIN_FEM_ENABLE, OUTPUT);
  digitalWrite(PIN_FEM_ENABLE, HIGH);
  pinMode(modePin(), OUTPUT);
  digitalWrite(modePin(), LOW);

  log_i("front end: %s on the antenna path (rail %d, enable %d, mode %d) — "
        "adds ~%d dB to the configured %d dBm drive",
        partName(), PIN_FEM_POWER, PIN_FEM_ENABLE, modePin(),
        gainDb(RF_TX_DBM), RF_TX_DBM);
}

void tx() {
  // High is "amplifier in the path" on both parts for transmit. The
  // transmit/receive select itself is on the radio's DIO2 and has already
  // switched by the time the PA sees a signal.
  digitalWrite(PIN_FEM_ENABLE, HIGH);
  digitalWrite(modePin(), HIGH);
}

void rx() {
  digitalWrite(PIN_FEM_ENABLE, HIGH);
  digitalWrite(modePin(), LOW);                 // LNA in the receive path
}

const char* partName() {
  switch (sPart) {
    case Part::GC1109:   return "GC1109";
    case Part::KCT8103L: return "KCT8103L";
    default:             return "none";
  }
}

int8_t gainDb(int8_t chipDbm) {
  if (sPart == Part::None) return 0;
  const int8_t* curve = sPart == Part::KCT8103L ? kKctGain : kGc1109Gain;
  const int8_t d = chipDbm < 0 ? 0 : (chipDbm > 21 ? 21 : chipDbm);
  return curve[d];
}

} // namespace LoRaFem

#endif // HAS_LORA_FEM
