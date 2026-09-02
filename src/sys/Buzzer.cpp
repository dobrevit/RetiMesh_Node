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

esp_timer_handle_t sTimer  = nullptr;
volatile bool      sBusy   = false;   // a note is on the air
volatile uint32_t  sNextHz = 0;       // and this one is owed after it (0: none)
uint32_t           sLenUs  = 0;       // note length of the current tune
portMUX_TYPE       sMux    = portMUX_INITIALIZER_UNLOCKED;

void tone(uint32_t hz) { ledcWriteTone(PIN_BUZZER, hz); }

// The state machine's whole content is "one more note may be owed" — a
// review found the tune-id/step pair encoding exactly that, and the claim
// below un-races it: start() runs on whichever task has news (the RNS task,
// mostly) while this runs on the esp_timer task.
void step(void*) {
  uint32_t next;
  portENTER_CRITICAL(&sMux);
  next = sNextHz;
  sNextHz = 0;
  if (!next) sBusy = false;
  portEXIT_CRITICAL(&sMux);
  if (next) {
    tone(next);
    esp_timer_start_once(sTimer, sLenUs);
  } else {
    tone(0);                                     // silence, and done
  }
}

// Start a sound if the sounder is idle. One at a time and none queued: the
// news a second sound would carry is already in the air.
bool start(uint32_t hz, uint32_t ms, uint32_t nextHz) {
  if (!sTimer) return false;
  portENTER_CRITICAL(&sMux);
  if (sBusy) { portEXIT_CRITICAL(&sMux); return false; }
  sBusy = true;
  sNextHz = nextHz;
  portEXIT_CRITICAL(&sMux);
  sLenUs = ms * 1000;
  tone(hz);
  esp_timer_start_once(sTimer, sLenUs);
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
void boot()    { start(2000, 120, 4000); }       // 2 kHz then 4 kHz
void message() { start(4000, 150, 0); }

} // namespace Buzzer

#endif // HAS_BUZZER
