// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  BoardInit.cpp — see BoardInit.h
// ============================================================================
#include "BoardInit.h"

#include <Arduino.h>
#include "Config.h"

namespace {

// A chip select at rest. Driven rather than merely configured, because the
// point is the level on the wire and not the direction of the pad.
inline void idleSelect(int pin) {
  if (pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);          // every device here deselects high
}

} // namespace

namespace BoardInit {

void begin() {
#if HAS_BOARD_POWER
  // The rail before anything else in setup() that could want it. The level is
  // the board's to say: a high-side switch wants a high, an active-low enable
  // wants a low, and both exist on boards this firmware runs on.
  pinMode(PIN_BOARD_POWER, OUTPUT);
  digitalWrite(PIN_BOARD_POWER, BOARD_POWER_ACTIVE);
  // Long enough for the switch to settle and the parts behind it to finish
  // their own resets before anyone addresses them. The keyboard's controller
  // is the slowest of them and wants longer still, which is why Keypad::begin
  // waits again rather than trusting this one number for everybody.
  delay(BOARD_POWER_SETTLE_MS);
  log_i("board rail on (GPIO %d -> %s)", PIN_BOARD_POWER,
        BOARD_POWER_ACTIVE == HIGH ? "high" : "low");
#endif

#if SPI_BUS_SHARED
  // Three devices, one set of wires. Each driver raises its own select in its
  // own begin(), which is correct and happens too late: whichever starts first
  // is talking on a bus where the other two selects are still floating, and a
  // floating select is a device that may answer. Idle all of them here, before
  // any of them exists.
  #if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_TFT
    const int panelCs = PIN_TFT_CS;
  #else
    const int panelCs = -1;
  #endif
  #if HAS_SD
    const int cardCs = PIN_SD_CS;
  #else
    const int cardCs = -1;
  #endif
  idleSelect(PIN_LORA_CS);
  idleSelect(panelCs);
  idleSelect(cardCs);
  // And the one line all three drive in turn: with every device deselected
  // nothing holds it, so it is pulled up rather than left to float into
  // whatever the first read makes of it.
  pinMode(PIN_LORA_MISO, INPUT_PULLUP);
  log_i("shared SPI bus: selects idled (radio %d, panel %d, card %d)",
        PIN_LORA_CS, panelCs, cardCs);
#endif
}

} // namespace BoardInit
