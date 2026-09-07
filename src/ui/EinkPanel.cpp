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
//  EinkPanel.cpp — see EinkPanel.h
// ============================================================================
#include "EinkPanel.h"

#if HAS_DISPLAY && DISPLAY_KIND == DISPLAY_KIND_EINK

#include <new>

// How long a panel may take to drop BUSY after a reset before we conclude
// there is nothing on the other end of the ribbon.
static constexpr uint32_t kBusyTimeoutMs = 2000;

// The driver waits on BUSY with no timeout of its own — a panel that never
// answers would spin inside its begin() forever, and that call sits in
// setup() before Wi-Fi, the radio, the web server and the console. So the
// line is asked first, the way OledPanel probes the I2C bus before handing
// the SSD1306 driver an address: a board with no panel has to come up
// headless and say so, not hang where nobody can see why.
//
// An absent panel leaves BUSY floating, so it is pulled up and reads high; a
// panel that is there drives it high while it wakes from reset and low when
// it is ready.
bool EinkPanel::panelAnswers() {
  pinMode(PIN_EPD_RST, OUTPUT);
  pinMode(PIN_EPD_BUSY, INPUT_PULLUP);
  digitalWrite(PIN_EPD_RST, LOW);
  delay(10);
  digitalWrite(PIN_EPD_RST, HIGH);
  const uint32_t deadline = millis() + kBusyTimeoutMs;
  while (digitalRead(PIN_EPD_BUSY) == HIGH && (int32_t)(millis() - deadline) < 0) delay(5);
  return digitalRead(PIN_EPD_BUSY) == LOW;
}

bool EinkPanel::begin() {
  // The panel sits on the switched peripheral rail, active low, as the OLED
  // boards' panels do, and it has to be up before the line below means
  // anything.
  panelVextOn();

  if (!panelAnswers()) {
    log_w("e-paper: BUSY never went low after reset (pin %d) — no panel, running headless",
          PIN_EPD_BUSY);
    return false;
  }

  // The driver first, because its constructor takes the larger allocation and
  // does it with a bare new[] it does not check; giving it the roomier heap
  // is the only influence we have over that. The canvas is ours and is
  // checked.
  _panel = new (std::nothrow) EinkDriver();
  _canvas = new (std::nothrow) GFXcanvas1(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (!_panel || !_canvas || !_canvas->getBuffer()) {
    log_e("e-paper: no room for a %dx%d frame — display disabled", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    delete _canvas; _canvas = nullptr;
    delete _panel;  _panel  = nullptr;
    return false;
  }

  _panel->begin();
  _panel->landscape();                 // 250 across, 122 down, as the layout says
  // White is the background the pages assume, and setting it here also means
  // every later call clears the page to it rather than to black.
  _panel->setBackgroundColor(WHITE);

  // The same invariants OledPanel establishes on its driver, on the surface
  // the pages actually draw on: a row one character too long is clipped, not
  // wrapped onto the reading below it.
  _canvas->setTextWrap(false);
  _canvas->setTextColor(ink());
  _canvas->fillScreen(paper());

  _ok = true;
  log_i("e-paper %dx%d up (CS %d, DC %d, RST %d, BUSY %d)",
        DISPLAY_WIDTH, DISPLAY_HEIGHT, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY);
  return _ok;
}

void EinkPanel::flush(bool full) {
  if (!_ok) return;
  // What a controller coming back from deep sleep owes this update, if it is
  // coming back from one at all (EinkSleep.h). Ordinarily nothing.
  const EinkSleep::Preamble pre = _sleep.beforeUpdate(full);
  // The reset the part requires before it will accept anything again. This
  // driver has no reset of its own to call — reset() is protected and is
  // reached only from a fastmode change, which does exactly the right thing:
  // Vext up, the reset pin pulled, BUSY waited on, then the software reset
  // and the waveform config. Forgetting which mode the controller is in is
  // how it is asked for one, because the change is what triggers the reload.
  if (pre.reload) _fastmode = -1;
  // Partial by default because a full pass flashes the panel; full when the
  // policy says the ghosting has had long enough, on the first frame, where
  // what is on the glass is the previous firmware's, and after a sleep, where
  // the difference the partial update is computed against may not have
  // survived it.
  //
  // Set only when it changes: each of these resets the controller, reloads
  // its waveform and waits on BUSY twice, which is not a thing to do before
  // every update on a panel whose updates are what we are rationing.
  const int8_t want = pre.full ? 0 : 1;
  if (want != _fastmode) {
    if (pre.full) _panel->fastmodeOff();
    else          _panel->fastmodeOn();
    _fastmode = want;
  }
  // Clear the page in memory, not on the glass. The obvious call for this,
  // clearMemory(), also pushes the blank frame to the controller a byte at a
  // time — four thousand SPI transactions immediately overwritten by the real
  // frame below. setBackgroundColor does the same local clear and no I/O.
  _panel->setBackgroundColor(WHITE);
  _panel->drawBitmap(0, 0, _canvas->getBuffer(), DISPLAY_WIDTH, DISPLAY_HEIGHT, BLACK);
  _panel->update();
}

// The glass keeps its picture and the controller stops. Those are two
// different things on this panel and only the first of them used to happen.
//
// The image stays, deliberately: blanking an e-paper panel by clearing it
// would throw away the reading a passer-by is meant to be able to take off a
// sleeping node, and it holds that image with no power at all. What was
// missing is that the part driving it does not stop when the picture does.
// The driver's update sequence already ends with the analog and the
// oscillator disabled — the master-activation option it sends, R22h = F7h for
// a full pass and FFh for a fast one, spells out "Enable clock signal, Enable
// Analog, Load temperature value, DISPLAY, Disable Analog, Disable OSC" — so
// what is left between updates is the controller's plain idle, not a running
// booster. The panel's own DC table puts that at tens of microamps and its
// deep sleep at about one, so this is a small, continuous saving rather than
// the milliamps a lit panel costs; it is worth taking on the one board whose
// selling point is drawing nothing while it stands still.
//
// It is not taken after every update, and that is a measurement rather than
// caution: coming back costs a hardware reset and a full refresh, and a full
// refresh on this panel is seconds of driving at milliamps — more charge than
// the deep sleep saves across the five minutes between resting updates. So
// the sleep is spent where nothing is waiting on the other side of it: a
// blank somebody asked for, after which the panel is not updated again until
// they ask for it back.
//
// The rail this panel hangs off is not an alternative here. On this board
// Vext feeds every peripheral on the PCB, the radio's front end included
// (the board header says so, and the driver library's own platform file calls
// it "power to Wireless Paper's interfaces (Display + LoRa P/A)"), so it can
// never drop on a transport node — and the driver raises it again on every
// mode change regardless. The controller's own deep sleep reaches the same
// place without touching the radio's supply.
void EinkPanel::blank(bool on) {
  if (!_ok) return;
  if (on) {
    // Idempotent because the screen edge is not: the same blank arrives from
    // a long press and from the page walker, and a controller already asleep
    // would not hear the command anyway.
    if (_sleep.sleep()) _panel->deepSleep();
    return;
  }
  // Nothing to send: the part leaves deep sleep only on a hardware reset, and
  // that reset belongs to the update that follows — flush() pays it, together
  // with the full refresh the lost differential reference needs. Waking here
  // instead would reset a panel that nobody has yet drawn anything for.
  _sleep.wake();
}

#endif // HAS_DISPLAY && EINK
