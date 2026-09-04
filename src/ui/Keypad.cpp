// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  Keypad.cpp — see Keypad.h
// ============================================================================
#include "Keypad.h"

#if HAS_KEYPAD || HAS_TRACKBALL

#include <Arduino.h>
#include <Wire.h>
#include "I2cReg.h"

namespace {

bool sPresent = false;

// ---------------------------------------------------------------------------
// The trackball
// ---------------------------------------------------------------------------
#if HAS_TRACKBALL

// One counter per direction, raised in an interrupt and drained by read().
// Counted rather than sampled because the ball emits a pulse per detent and
// says nothing between them: a poll at the display's rate would see a level,
// not the movement, and a quick roll would arrive as one step or none.
volatile uint32_t sTicks[4] = {0, 0, 0, 0};   // up, down, left, right

// The ball is geared finely enough that one detent per key would send the
// cursor across the screen on the lightest touch. The reference firmware for
// this board settles on three, and so does this.
constexpr uint32_t kDetentsPerKey = 3;

void IRAM_ATTR isrUp()    { sTicks[0]++; }
void IRAM_ATTR isrDown()  { sTicks[1]++; }
void IRAM_ATTR isrLeft()  { sTicks[2]++; }
void IRAM_ATTR isrRight() { sTicks[3]++; }

void trackballBegin() {
  struct Line { int pin; void (*isr)(); };
  const Line lines[4] = {
    { PIN_TRACKBALL_UP,    isrUp    },
    { PIN_TRACKBALL_DOWN,  isrDown  },
    { PIN_TRACKBALL_LEFT,  isrLeft  },
    { PIN_TRACKBALL_RIGHT, isrRight },
  };
  for (const Line& l : lines) {
    if (l.pin < 0) continue;
    pinMode(l.pin, INPUT_PULLUP);            // idle high, pulses low
    attachInterrupt(digitalPinToInterrupt(l.pin), l.isr, FALLING);
  }
  log_i("trackball armed (up %d, down %d, left %d, right %d; %lu detents per key)",
        PIN_TRACKBALL_UP, PIN_TRACKBALL_DOWN, PIN_TRACKBALL_LEFT, PIN_TRACKBALL_RIGHT,
        (unsigned long)kDetentsPerKey);
}

// One direction's worth of movement, or KEY_NONE. Takes the whole quota out of
// the counter rather than clearing it, so a roll that outran the reader keeps
// its remaining steps for the next call instead of being thrown away.
uint8_t trackballRead() {
  static const uint8_t keys[4] = { Keypad::KEY_UP, Keypad::KEY_DOWN,
                                   Keypad::KEY_LEFT, Keypad::KEY_RIGHT };
  for (uint8_t i = 0; i < 4; i++) {
    if (sTicks[i] < kDetentsPerKey) continue;
    noInterrupts();
    sTicks[i] -= kDetentsPerKey;
    interrupts();
    return keys[i];
  }
  return Keypad::KEY_NONE;
}

#endif // HAS_TRACKBALL

// ---------------------------------------------------------------------------
// The keyboard
// ---------------------------------------------------------------------------
#if HAS_KEYPAD

// One bare byte per read, no register to address first: the controller's own
// firmware answers a read request with the key pressed since the last one, or
// with 0x00 for none. That is the whole protocol — there is no held state to
// poll and no release to wait for, so a missed read is a lost keystroke and
// the reader has to be called often enough to keep up with typing.
uint8_t keyboardRead() {
  TwoWire& bus = I2cReg::mainBus();
  if (bus.requestFrom((uint8_t)KEYPAD_ADDR, (uint8_t)1) != 1) return Keypad::KEY_NONE;
  const uint8_t k = bus.read();
  return k;
}

void keyboardBegin() {
  TwoWire& bus = I2cReg::mainBus();
  // The controller takes a moment after the rail comes up before it answers,
  // and the rail was raised at the top of setup() — long ago in the boot's
  // terms, but this is the one part on the board that is slow enough to be
  // worth checking rather than assuming.
  for (uint8_t attempt = 0; attempt < 10 && !sPresent; attempt++) {
    bus.beginTransmission((uint8_t)KEYPAD_ADDR);
    if (bus.endTransmission() == 0) { sPresent = true; break; }
    delay(50);
  }
  if (sPresent) {
    log_i("keyboard at 0x%02x (SDA %d, SCL %d)", KEYPAD_ADDR, PIN_KEYPAD_SDA, PIN_KEYPAD_SCL);
    // Drain whatever was typed while the node was booting, so the first thing
    // the shell sees is a key the operator meant for it.
    for (uint8_t i = 0; i < 8 && keyboardRead() != Keypad::KEY_NONE; i++) {}
  } else {
    log_w("keyboard did not answer at 0x%02x — the on-glass keyboard stays", KEYPAD_ADDR);
  }
}

#endif // HAS_KEYPAD

} // namespace

namespace Keypad {

void begin() {
#if HAS_TRACKBALL
  trackballBegin();
#endif
#if HAS_KEYPAD
  keyboardBegin();
#endif
}

uint8_t read() {
  // The ball first, and cheaply: it is a counter in RAM, while the keyboard is
  // a transaction on a bus shared with the touch controller. Nothing is
  // starved by the order — a key held down is still one key per call either
  // way, and the ball only wins while it is actually turning.
#if HAS_TRACKBALL
  const uint8_t nav = trackballRead();
  if (nav != KEY_NONE) return nav;
#endif
#if HAS_KEYPAD
  if (sPresent) return keyboardRead();
#endif
  return KEY_NONE;
}

bool present() { return sPresent; }

} // namespace Keypad

#endif // HAS_KEYPAD || HAS_TRACKBALL
