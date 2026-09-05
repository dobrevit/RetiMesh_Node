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
//  Two parts answer the same two events, and neither may make a caller wait —
//  message() is called from the RNS task, which is carrying packets.
//
//  The piezo is a PWM pin: a call starts the first note and arms an esp_timer
//  for what happens next, and the timer callback does a register write on the
//  esp_timer task. Nothing waits anywhere.
//
//  The speaker cannot be driven that way. An I2S amplifier wants a stream of
//  samples for as long as the note lasts, so the note is played by a task of
//  its own and a caller posts to it and returns. That task is the whole cost of
//  the difference — a stack and a sample buffer, on a board whose internal RAM
//  is the scarce kind — which is why it exists only where a board asks for it.
// ============================================================================
#include "Buzzer.h"

#if HAS_BUZZER

#include <Arduino.h>
#include <esp_timer.h>
#include "Settings.h"

#if BUZZER_KIND == BUZZER_KIND_I2S
#include <ESP_I2S.h>
#include "Diag.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#endif

namespace {

#if BUZZER_KIND == BUZZER_KIND_I2S
// ---------------------------------------------------------------------------
// A speaker behind an I2S amplifier
// ---------------------------------------------------------------------------
// Eight kilohertz, mono, sixteen bits. A beep needs no more, and the rate sets
// the size of everything below it: the buffer, the DMA behind it, and the time
// a caller could ever be made to wait.
constexpr uint32_t kRateHz  = 8000;
// A quarter of full scale. The MAX98357A on the board this was written for
// straps its gain pin to ground, which is 12 dB, and full scale through that
// into a small speaker is startling rather than informative.
constexpr int16_t  kAmplitudeFull = 8000;
// The amplitude actually used, from the volume setting. A quarter of full scale
// is the ceiling rather than the level: the MAX98357A straps its gain to ground,
// which is 12 dB, and full scale through that into a small speaker is startling
// rather than informative.
int16_t sAmplitude = kAmplitudeFull;
// Square rather than sine, deliberately: it is what the piezo this stands in
// for produces, it needs no floating point in the task that generates it, and
// the difference is inaudible in a 120 ms beep.
constexpr size_t   kChunk   = 256;            // samples generated at a time

I2SClass      sI2s;
QueueHandle_t sQueue = nullptr;
bool          sReady = false;
// Whether a sound is on the air, which the queue cannot answer. The task takes
// a note off the queue before playing it, so an empty queue means "playing" as
// often as it means "idle" — and a depth that let a second note wait would
// contradict what this file promises anyway. Held here and tested under the
// same kind of claim the piezo uses, because start() runs on whichever task has
// news and this is cleared on the audio task.
volatile bool sBusy = false;
portMUX_TYPE  sBusyMux = portMUX_INITIALIZER_UNLOCKED;

struct Note { uint32_t hz; uint32_t ms; uint32_t nextHz; };

// One note into the amplifier, generated a chunk at a time so that the buffer
// stays small whatever the note's length.
void play(uint32_t hz, uint32_t ms) {
  if (!hz || !ms) return;
  int16_t buf[kChunk];
  const uint32_t halfPeriod = kRateHz / (hz * 2);        // samples per half cycle
  uint32_t phase = 0;
  size_t   left  = (size_t)((uint64_t)kRateHz * ms / 1000);
  while (left) {
    const size_t n = left < kChunk ? left : kChunk;
    for (size_t i = 0; i < n; i++) {
      buf[i] = (halfPeriod && (phase / halfPeriod) % 2) ? sAmplitude : (int16_t)-sAmplitude;
      phase++;
    }
    sI2s.write((const uint8_t*)buf, n * sizeof(int16_t));
    left -= n;
  }
  // A note ends on silence rather than on whatever sample it reached: leaving
  // the last level in the amplifier is a step the speaker reproduces as a click.
  int16_t quiet[32] = {0};
  sI2s.write((const uint8_t*)quiet, sizeof(quiet));
}

// Asked to stand down, and the acknowledgement. The task deletes itself rather
// than being deleted: it can be inside an I2S write when the switch is thrown,
// and killing a task mid-driver leaves the driver's state to chance. A sentinel
// note reaches it only between notes, which is exactly when it is safe to go.
volatile bool sStopping = false;
volatile bool sStopped  = false;

void audioTask(void*) {
  Note n;
  for (;;) {
    if (xQueueReceive(sQueue, &n, portMAX_DELAY) != pdTRUE) continue;
    if (!n.hz && !n.ms) {          // the sentinel: nothing to play, time to go
      sStopped = true;
      vTaskDelete(nullptr);
      return;
    }
    play(n.hz, n.ms);
    if (n.nextHz) play(n.nextHz, n.ms);
    // Only now. Clearing it when the note was taken off the queue would open
    // the door for the length of the sound, which is precisely the window in
    // which a second one arrives.
    portENTER_CRITICAL(&sBusyMux);
    sBusy = false;
    portEXIT_CRITICAL(&sBusyMux);
  }
}

// Started if the sounder is idle, refused otherwise: one at a time and none
// queued, which is what Buzzer.h promises and what the piezo does. The news a
// second sound would carry is the news already in the air.
//
// The claim is made before the note is posted rather than after, because the
// task can finish a short note and clear it between the two — and a start()
// that then posted would leave a note on the queue with nothing claiming to be
// playing it.
bool start(uint32_t hz, uint32_t ms, uint32_t nextHz) {
  if (!sReady || !sQueue) return false;
  portENTER_CRITICAL(&sBusyMux);
  if (sBusy) { portEXIT_CRITICAL(&sBusyMux); return false; }
  sBusy = true;
  portEXIT_CRITICAL(&sBusyMux);

  const Note n{hz, ms, nextHz};
  if (xQueueSend(sQueue, &n, 0) == pdTRUE) return true;
  // It did not go: give the claim back rather than leaving the sounder marked
  // busy for ever over a note nobody will play.
  portENTER_CRITICAL(&sBusyMux);
  sBusy = false;
  portEXIT_CRITICAL(&sBusyMux);
  return false;
}

#else
// ---------------------------------------------------------------------------
// A piezo on a PWM pin
// ---------------------------------------------------------------------------
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
#endif // BUZZER_KIND

} // namespace

namespace Buzzer {

namespace {

// Volume as an amplitude, clamped. Zero is silence rather than a click: a note
// of zero amplitude still costs the time it takes to play.
void setVolume(uint8_t percent) {
#if BUZZER_KIND == BUZZER_KIND_I2S
  if (percent > 100) percent = 100;
  sAmplitude = (int16_t)((int32_t)kAmplitudeFull * percent / 100);
#else
  // A piezo on a PWM channel has one loudness: the pin swings rail to rail and
  // the element is as loud as it is. Duty cycle changes the timbre rather than
  // the level, and driving it quieter would mean driving it wrong. The setting
  // is accepted and ignored here rather than hidden, so a fleet can carry one
  // configuration across boards that answer it differently.
  (void)percent;
#endif
}

bool sUp = false;              // the hardware is taken up

void takeUp();
void release();

} // namespace

void begin() {
  setVolume(settings.sound().volume);
  apply();
}

// Match the hardware to the setting. Idempotent, so the settings layer can call
// it after every save without knowing whether anything changed.
void apply() {
  setVolume(settings.sound().volume);
  const bool want = settings.sound().enabled;
  if (want == sUp) return;
  if (want) takeUp(); else release();
}

bool present() { return sUp; }

namespace {

void takeUp() {
#if BUZZER_KIND == BUZZER_KIND_I2S
  sI2s.setPins(PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
  if (!sI2s.begin(I2S_MODE_STD, kRateHz, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    log_w("buzzer: I2S would not start on BCLK %d, LRCLK %d, DOUT %d",
          PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
    return;
  }
  sQueue = xQueueCreate(1, sizeof(Note));        // one at a time, and none owed
  if (!sQueue) { sI2s.end(); return; }
  // Through Diag rather than straight to FreeRTOS: it reports a task that would
  // not start the way every other one here is reported, and it is the list this
  // task's stack headroom is watched from.
  //
  // Small and low: it spends its life blocked on the queue, and what it does
  // when it wakes is fill a buffer with two alternating values.
  if (!Diag::startTask(audioTask, "audio", 2560, nullptr, 1, 0)) {
    vQueueDelete(sQueue);
    sQueue = nullptr;
    sI2s.end();
    return;
  }
  sReady = true;
  log_i("buzzer: I2S speaker on BCLK %d, LRCLK %d, DOUT %d",
        PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DOUT);
#else
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
#endif
  sUp = true;
}

// Give the hardware back. On the speaker this is the whole reason the switch
// exists — the task's stack and the DMA ring are the cost, and a switch that
// only silenced them would save nothing.
void release() {
#if BUZZER_KIND == BUZZER_KIND_I2S
  sReady = false;                    // refuse new notes before anything goes
  if (sQueue) {
    sStopping = true;
    sStopped = false;
    const Note bye{0, 0, 0};
    // Room for it: a note in flight is at most a couple of hundred
    // milliseconds, and the queue is one deep.
    if (xQueueSend(sQueue, &bye, pdMS_TO_TICKS(500)) == pdTRUE) {
      for (int i = 0; i < 100 && !sStopped; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!sStopped) {
      // It did not go. Leave the queue and the driver alone rather than pull
      // them out from under a task that is still running in them: the sounder
      // stays up and the memory stays spent, which is the safe half of a bad
      // pair.
      log_w("buzzer: the audio task did not stand down; leaving the speaker up");
      sStopping = false;
      sReady = true;
      return;
    }
    vQueueDelete(sQueue);
    sQueue = nullptr;
  }
  sStopping = false;
  sI2s.end();
  portENTER_CRITICAL(&sBusyMux);
  sBusy = false;                     // whatever was playing is not any more
  portEXIT_CRITICAL(&sBusyMux);
  log_i("buzzer: speaker released; its task and DMA are back");
#else
  if (sTimer) { esp_timer_stop(sTimer); esp_timer_delete(sTimer); sTimer = nullptr; }
  ledcWriteTone(PIN_BUZZER, 0);
  ledcDetach(PIN_BUZZER);
  sBusy = false;
#endif
  sUp = false;
}

} // namespace

// Near a small piezo's resonance, short enough to be polite. Worth saying:
// the first bench V4 produced no sound at any drive or frequency while every
// other function worked, so its sounder is likely simply not fitted — the pin
// is driven regardless, because a board that has one should be heard and one
// that does not loses nothing.
void boot()    { start(2000, 120, 4000); }       // 2 kHz then 4 kHz
void message() { start(4000, 150, 0); }

} // namespace Buzzer

#endif // HAS_BUZZER
