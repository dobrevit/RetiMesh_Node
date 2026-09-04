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

// The last few codes the controller sent, before this file decides what any of
// them mean. A board's function keys are whatever its own microcontroller
// numbers them, and nothing published says which — so the way to learn them is
// to press them and read them back, rather than to guess and map a key to the
// wrong screen. Kept small and never cleared: a bring-up presses six buttons in
// a row and reads them in one go — deep enough that a few stray presses
// beforehand do not push the run of interest out of it.
constexpr uint8_t kRawKept = 24;
uint8_t  sRaw[kRawKept] = {0};
uint8_t  sRawAt = 0;
uint32_t sRawCount = 0;

void recordRaw(uint8_t code) {
  if (!code) return;
  sRaw[sRawAt] = code;
  sRawAt = (uint8_t)((sRawAt + 1) % kRawKept);
  sRawCount++;
}

// ---------------------------------------------------------------------------
// The trackball
// ---------------------------------------------------------------------------
#if HAS_TRACKBALL

// One counter per direction, raised in an interrupt and drained by read().
// Counted rather than sampled because the ball emits a pulse per detent and
// says nothing between them: a poll at the display's rate would see a level,
// not the movement, and a quick roll would arrive as one step or none.
volatile uint32_t sTicks[4] = {0, 0, 0, 0};   // up, down, left, right

// A spinlock rather than noInterrupts(), because the two sides are not on one
// core: attachInterrupt() runs from setup() on the Arduino core and registers
// the handler there, while read() is called from the display task, which
// main.cpp pins to the other one. Masking interrupts locally excludes nothing
// on the core the pulses arrive on, and the counter's read-modify-write below
// then loses a detent to whichever side wrote last.
portMUX_TYPE sTicksMux = portMUX_INITIALIZER_UNLOCKED;

// The ball is geared finely enough that one detent per key would send the
// cursor across the screen on the lightest touch. The reference firmware for
// this board settles on three, and so does this.
constexpr uint32_t kDetentsPerKey = 3;

inline void IRAM_ATTR tick(uint8_t i) {
  taskENTER_CRITICAL_ISR(&sTicksMux);
  sTicks[i]++;
  taskEXIT_CRITICAL_ISR(&sTicksMux);
}

void IRAM_ATTR isrUp()    { tick(0); }
void IRAM_ATTR isrDown()  { tick(1); }
void IRAM_ATTR isrLeft()  { tick(2); }
void IRAM_ATTR isrRight() { tick(3); }

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
    bool took = false;
    taskENTER_CRITICAL(&sTicksMux);
    if (sTicks[i] >= kDetentsPerKey) { sTicks[i] -= kDetentsPerKey; took = true; }
    taskEXIT_CRITICAL(&sTicksMux);
    if (took) return keys[i];
  }
  return Keypad::KEY_NONE;
}

#endif // HAS_TRACKBALL

// ---------------------------------------------------------------------------
// The keyboard
// ---------------------------------------------------------------------------
#if HAS_KEYPAD

// Which bus the controller is on. Where its pins are the board's general I2C
// it shares that bus; where they are not, the keyboard has a pair to itself.
// Both arrangements exist on boards here, and the difference is entirely in
// which object the transactions go through.
//
// Which host that is, and starting it, is I2cReg's to answer — the touch layer
// asks the same question about its own pins, and one place has to hold the
// answer or both drivers claim I2C host 1 and the second begin() re-initialises
// the bus under the first. See I2cReg::busFor().
inline TwoWire& keypadBus() {
  return I2cReg::busFor(PIN_KEYPAD_SDA, PIN_KEYPAD_SCL, KEYPAD_HZ);
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
    // The six shortcut keys along the case, in the numbering its controller
    // actually uses. Read off the hardware rather than taken from a published
    // map: pressing them in the order the case labels them — chats, home, menu,
    // back, location, map — returned 81 82 83 86 84 85, which is not the order
    // the numbers suggest and not what the one third-party map of this
    // controller claims either.
    case 0x81: return Keypad::KEY_MESSAGES;
    case 0x82: return Keypad::KEY_HOME;
    case 0x83: return Keypad::KEY_MENU;
    case 0x84: return Keypad::KEY_GPS;
    case 0x85: return Keypad::KEY_MAP;
    case 0x86: return Keypad::KEY_BACK;
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
  recordRaw((uint8_t)v);
  return translate((uint8_t)v);
#else
  if (bus.requestFrom((uint8_t)KEYPAD_ADDR, (uint8_t)1) != 1) return Keypad::KEY_NONE;
  const uint8_t k = bus.read();
  // 0xFF is a failed read, not a key — the same reading an idle bus with
  // nobody driving it gives. Taken as a key it is a keypress four times a
  // second for ever, which is a panel that wakes itself the moment it blanks
  // and a cell spent overnight.
  if (k == 0xFF) return Keypad::KEY_NONE;
  recordRaw(k);
  return k;
#endif
}

void keyboardBegin() {
  // Up already where the keyboard shares the board's bus, up now where it has
  // one of its own — either way this is the call that starts it.
  bool busUp = false;
  I2cReg::busFor(PIN_KEYPAD_SDA, PIN_KEYPAD_SCL, KEYPAD_HZ, &busUp);
  if (!busUp) {
    log_w("keyboard: bus would not start (SDA %d, SCL %d)", PIN_KEYPAD_SDA, PIN_KEYPAD_SCL);
    return;
  }
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
    // ...and out of the record with it: recentRaw() exists so that a bring-up
    // can press six buttons in a known order and read them back, and a boot
    // backlog at the head of that list is exactly what would be misread.
    sRawAt = 0; sRawCount = 0;
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

size_t recentRaw(uint8_t* out, size_t max) {
  // Oldest first, so a bring-up that presses six buttons in order reads them
  // back in that order.
  const size_t have = sRawCount < kRawKept ? (size_t)sRawCount : kRawKept;
  const size_t start = sRawCount < kRawKept ? 0 : sRawAt;
  size_t n = 0;
  for (size_t i = 0; i < have && n < max; i++) out[n++] = sRaw[(start + i) % kRawKept];
  return n;
}

} // namespace Keypad

#endif // HAS_KEYPAD || HAS_TRACKBALL
