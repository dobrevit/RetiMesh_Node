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
//  Buzzer.cpp — see Buzzer.h
//
//  Timing without blocking: each call starts the first note and arms an
//  esp_timer for what happens next — the second note, or silence. The timer
//  callback runs on the esp_timer task, where ledcWriteTone is a register
//  write and safe. A caller never waits.
// ============================================================================
#include "Buzzer.h"

#if HAS_BUZZER

#include <Arduino.h>
#include <esp_timer.h>

namespace {

esp_timer_handle_t sTimer = nullptr;
volatile uint8_t   sStep  = 0;   // where in the current little tune we are
volatile uint8_t   sTune  = 0;   // 0 idle, 1 boot pair, 2 message note

void tone(uint32_t hz) { ledcWriteTone(PIN_BUZZER, hz); }

void step(void*) {
  // The boot pair: first note has ended, either start the second or finish.
  if (sTune == 1 && sStep == 1) {
    sStep = 2;
    tone(4000);                                  // up to the piezo's sweet spot
    esp_timer_start_once(sTimer, 120 * 1000);
    return;
  }
  tone(0);                                       // silence, and done
  sTune = 0;
  sStep = 0;
}

// Start a tune if the sounder is idle. One at a time and none queued: the
// news a second sound would carry is already in the air.
bool start(uint8_t tune, uint32_t hz, uint32_t ms) {
  if (!sTimer || sTune) return false;
  sTune = tune;
  sStep = 1;
  tone(hz);
  esp_timer_start_once(sTimer, ms * 1000);
  return true;
}

} // namespace

namespace Buzzer {

void begin() {
  if (!ledcAttach(PIN_BUZZER, 2000, 10)) {       // any tone re-tunes from here
    log_w("buzzer: pin %d would not take a PWM channel", PIN_BUZZER);
    return;
  }
  ledcWriteTone(PIN_BUZZER, 0);                  // claimed, silent
  const esp_timer_create_args_t args = {
    .callback = step, .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK, .name = "buzzer", .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &sTimer) != ESP_OK) sTimer = nullptr;
}

// Near a small piezo's resonance, short enough to be polite. Worth saying:
// the first bench V4 produced no sound at any drive or frequency while every
// other function worked, so its sounder is likely simply not fitted — the pin
// is driven regardless, because a board that has one should be heard and one
// that does not loses nothing.
void boot()    { start(1, 2000, 120); }          // 2 kHz then 4 kHz
void message() { start(2, 4000, 150); }

} // namespace Buzzer

#endif // HAS_BUZZER
