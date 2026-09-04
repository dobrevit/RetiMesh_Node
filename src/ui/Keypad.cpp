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

// Which bus the controller is on. Where its pins are the board's general I2C
// it shares that bus; where they are not, the keyboard has a pair to itself
// and this file owns it. Both arrangements exist on boards here, and the
// difference is entirely in which object the transactions go through.
#if (PIN_KEYPAD_SDA == PIN_I2C_SDA) && (PIN_KEYPAD_SCL == PIN_I2C_SCL)
  #define KEYPAD_ON_MAIN_I2C 1
#else
  #define KEYPAD_ON_MAIN_I2C 0
TwoWire sKeypadBus(1);
#endif

inline TwoWire& keypadBus() {
#if KEYPAD_ON_MAIN_I2C
  return I2cReg::mainBus();
#else
  return sKeypadBus;
#endif
}

// One key per read either way; what differs is how it is asked for.
//
// BARE: the controller answers a read request with the key pressed since the
// last one, or 0x00. There is no register to address and none to get wrong.
//
// REG8: the controller is a register file, so the key register is written
// first and read back with a repeated start. A bare read against this one
// returns whatever its pointer was left sitting on, which is why the protocol
// is declared by the board rather than discovered.
//
// Neither has a held state or a release event, so a missed read is a lost
// keystroke and the reader has to be called often enough to keep up with
// typing. Both report "nothing" as zero; one of the two controllers also uses
// 0xFF for a failed read, which is not a key on either.
#if KEYPAD_KIND == KEYPAD_KIND_REG8
// The register-file controller sends its arrow keys and its function keys
// above ASCII, in its own numbering. Translated here rather than in the shell:
// what a controller calls its keys is a fact about the controller, and every
// consumer above this line should see one vocabulary whatever board it is on.
// Codes this firmware has no meaning for are dropped rather than passed up as
// stray characters — including the controller's own "not a key" sentinel.
uint8_t translate(uint8_t raw) {
  switch (raw) {
    case 0xB4: return Keypad::KEY_LEFT;
    case 0xB5: return Keypad::KEY_UP;
    case 0xB6: return Keypad::KEY_DOWN;
    case 0xB7: return Keypad::KEY_RIGHT;
    case 0x88: return Keypad::KEY_NONE;      // the controller's invalid-key marker
    default:   break;
  }
  // Everything printable, plus the three control codes this firmware acts on,
  // is already what it looks like.
  if (raw == Keypad::KEY_BACKSPACE || raw == Keypad::KEY_ENTER ||
      raw == Keypad::KEY_ESC || (raw >= 0x20 && raw <= 0x7E))
    return raw;
  return Keypad::KEY_NONE;
}
#endif

uint8_t keyboardRead() {
  TwoWire& bus = keypadBus();
#if KEYPAD_KIND == KEYPAD_KIND_REG8
  const int v = I2cReg::read(bus, (uint8_t)KEYPAD_ADDR, (uint8_t)KEYPAD_KEY_REG);
  if (v < 0 || v == 0xFF) return Keypad::KEY_NONE;
  return translate((uint8_t)v);
#else
  if (bus.requestFrom((uint8_t)KEYPAD_ADDR, (uint8_t)1) != 1) return Keypad::KEY_NONE;
  return bus.read();
#endif
}

void keyboardBegin() {
#if !KEYPAD_ON_MAIN_I2C
  if (!sKeypadBus.begin(PIN_KEYPAD_SDA, PIN_KEYPAD_SCL, KEYPAD_HZ)) {
    log_w("keyboard: bus would not start (SDA %d, SCL %d)", PIN_KEYPAD_SDA, PIN_KEYPAD_SCL);
    return;
  }
#endif
#if PIN_KEYPAD_LED >= 0
  // The keyboard's own backlight, driven from this chip rather than asked of
  // the controller. Off until somebody wants it: it is the second-largest
  // draw on the board after the panel.
  pinMode(PIN_KEYPAD_LED, OUTPUT);
  digitalWrite(PIN_KEYPAD_LED, LOW);
#endif
  TwoWire& bus = keypadBus();
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
