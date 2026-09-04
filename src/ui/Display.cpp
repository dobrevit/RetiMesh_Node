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
//  Display.cpp — see Display.h
// ============================================================================
#include "Diag.h"
#include "Display.h"
#include "DisplayLayout.h"
#include "VersionLabel.h"
#include "esp32-hal-periman.h"
#include <WiFi.h>
#include "WifiManager.h"
#include "Settings.h"
#include "Neighbors.h"
#include "RnsAnnounce.h"
#include "SdCard.h"
#include "RnsTransport.h"
#include "Power.h"
#include "Gps.h"
#include "LoRaRadio.h"        // loraQualityPercent, for the signal meter

Display display;

#if HAS_DISPLAY
#include "DisplayIcons.h"
#include "QrCode.h"
#include "TouchInput.h"
#include "LvglUi.h"
#include "Bootloader.h"
#include "Bq25896.h"
#include "Imu.h"
#include <esp_sleep.h>
#include "LxmfInbox.h"
#include "OtaUpdate.h"
#include "Watchdog.h"

#if HAS_LVGL_UI
// The runtime half of HAS_LVGL_UI: begin() promised a fall-back to the mono
// pages when the shell cannot start, and the compile-time branch alone made
// that promise a lie — the task ran shell calls against a shell that never
// existed. Everything GUI asks this first.
static bool sShellUp = false;
#endif


bool Display::begin() {
#if DISPLAY_KIND == DISPLAY_KIND_OLED || DISPLAY_KIND == DISPLAY_KIND_EINK || \
    DISPLAY_KIND == DISPLAY_KIND_TFT
  _panel = &_panelImpl;
#endif
  if (!_panel || !_panel->begin()) return false;
  _gfx = &_panel->gfx();
  _ok  = true;

  // Something on the glass before the first page is drawn: the panel holds
  // whatever the last firmware left in it otherwise, and on e-paper that
  // survives a power cut.
  TouchInput::begin();                   // input for a panel someone can now see

#if HAS_LVGL_UI
  // The shell owns the glass from the first frame: pages, splash and the
  // refresh policy stay out of its way. The mono page stack below remains
  // exactly what every other board runs.
  sShellUp = LvglUi::begin(_panelImpl);
  if (sShellUp) return _ok;
  log_w("display: the GUI shell could not start; falling back to the pages");
#endif
  _panel->clear();
  _gfx->setCursor(0, 0);
  _gfx->print(FW_NAME);
  _gfx->setCursor(0, DisplayLayout::rowY(0));
  _gfx->print("booting...");
  _panel->flush(true);
  _refresh.forget();                     // that frame was not a page
  return _ok;
}

void Display::notice(const char* title, const char* detail) {
  if (!_ok) return;
  const auto l = DisplayLayout::active();
  _panel->clear();
  _gfx->setTextColor(_panel->ink());
  _gfx->setTextSize(1);
  // Centred, because a notice is the only thing on the panel and a left-aligned
  // line on a 64-pixel display looks like a page that failed to finish drawing.
  auto centre = [&](const char* text, int16_t y) {
    if (!text || !*text) return;
    const int16_t w = (int16_t)strlen(text) * DisplayLayout::FONT_W;
    int16_t x = (int16_t)((l.width - w) / 2);
    if (x < 0) x = 0;
    _gfx->setCursor(x, y);
    _gfx->print(text);
  };
  const int16_t mid = (int16_t)(l.height / 2);
  centre(title,  (int16_t)(detail ? mid - DisplayLayout::FONT_H : mid - DisplayLayout::FONT_H / 2));
  centre(detail, (int16_t)(mid + 1));
  // Straight to the glass, whatever the policy would have said: a notice is
  // shown because a person is waiting on it. What the page model believes is
  // on the panel is no longer true afterwards.
  _panel->flush(true);
  _refresh.forget();
}

void Display::displayTask(void* self) {
  auto* d = static_cast<Display*>(self);
  if (!d->_ok) { vTaskDelete(nullptr); return; }
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if HAS_BUTTON2
  pinMode(PIN_BUTTON2, INPUT_PULLUP);
#endif
  d->_lastActivityMs = millis();
  Watchdog::watch();
  for (;;) {
    // Fed before the pass, not after: an e-paper full refresh is seconds long
    // and is the slowest thing WATCHDOG_TIMEOUT_S has to clear.
    Watchdog::feed();
    // A page that cannot allocate skips that pass rather than taking the node
    // with it: the display is the least important thing on a node under
    // pressure and must be the first to give way (Diag.h).
    Diag::guard("the display task", [d] {
    d->pollButton();
#if !HAS_LVGL_UI && HAS_TOUCH
    d->pollTouch();                      // the shell reads the glass itself
#endif
#if HAS_BUTTON2
    d->pollButton2();
#endif
    uint32_t now = millis();
#if HAS_LVGL_UI
    {
      // The brightness setting reaches the glass here, once per change.
      static uint8_t lastB = 255;
      const uint8_t b = settings.display().brightness;
      if (b != lastB) { lastB = b; d->_panelImpl.setBrightness(b); }
    }
    if (sShellUp) {
      // The rest-and-alarm policy is the shell's own (LvglUi::restTick);
      // the panel applies the verdict and keeps the backlight to itself.
      switch (LvglUi::restTick(now, d->_lastActivityMs, d->_blank,
                               d->_panel->blanks())) {
        case LvglUi::PanelAction::Wake:
          if (d->_blank) d->setBlank(false);
          break;
        case LvglUi::PanelAction::Sleep: d->setBlank(true); break;
        case LvglUi::PanelAction::None:  break;
      }
    } else
#endif
    // Battery saving, where there is any to save: an OLED comes off with its
    // charge pump, an e-paper panel holds its image for nothing — one
    // statement of the rule for the mono pages and a shell that failed.
    if (d->_panel->blanks() && !d->_blank &&
        now - d->_lastActivityMs > Power::displaySleepMs()) d->setBlank(true);
#if HAS_LVGL_UI
    if (sShellUp) {
    // The shell's pass: LVGL timers, then done — the page machinery below
    // belongs to the mono boards and to a shell that failed to start. And a
    // blanked glass wakes to a tap the way a phone does: while dark, the
    // touch controller is polled gently here, since the shell's loop is
    // stopped.
    switch (LvglUi::takePowerAction()) {
      case 1: d->setBlank(true); break;
      case 2: Bootloader::reboot(Bootloader::Source::Ui); break;
      case 3:
        // The glass first: deep sleep holds the pins as they stand, and
        // "off" must not leave a lit backlight showing a frozen frame. Then
        // ship mode if the charger answers; the deepest sleep the chip has
        // otherwise, with the user button as the way back.
        d->setBlank(true);
        Bq25896::shipMode();
        esp_sleep_enable_ext1_wakeup(1ULL << PIN_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
        break;
      default: break;
    }
#if HAS_DA217
    // The panel follows the hand: two agreeing readings a second apart turn
    // the display, so a wobble costs nothing and a real turn costs a second.
    {
      static uint32_t lastImuMs = 0;
      static Imu::Facing lastF = Imu::Facing::Unknown;
      if (!d->_blank && now - lastImuMs >= 1000) {   // a dark panel needs no orienting
        lastImuMs = now;
        const Imu::Facing f = Imu::facing();
        if (f == lastF && f != Imu::Facing::Flat && f != Imu::Facing::Unknown)
          LvglUi::setRotation((uint8_t)f);
        lastF = f;
      }
    }
#endif
    if (d->_blank) {
      static uint32_t lastWakePoll = 0;
      // 250 ms: a real tap lasts longer than that, and at 100 ms a node that
      // sleeps all day burned ten doomed bus reads a second to notice one.
      if (settings.display().touchWake && now - lastWakePoll >= 250) {
        lastWakePoll = now;
        if (TouchInput::poll().down) {
          // Activity first — the bench found the wake flickering and dying:
          // without this the timer was still expired on the very next pass
          // and re-blanked the panel under the waking finger. And the tap
          // that wakes must only wake: the shell ignores this contact until
          // it lifts.
          d->_lastActivityMs = now;
          LvglUi::swallowTouch();
          d->setBlank(false);
        }
      }
    } else {
      LvglUi::loop();
    }
    return;
    }                                    // !sShellUp falls through to the pages
#endif
    if (d->_page != STATUS && now - d->_pageChangedMs > DISPLAY_PAGE_TIMEOUT_MS) {
      d->_page = STATUS; d->_pageChangedMs = now;
      d->_refresh.interval(d->cadence());    // back to what the node rests at
      // And shown now: this is the same transition a press makes, so it needs
      // the same exemption. Without it the frame is refused by the gap that
      // was just installed, and a panel that holds its image would sit on the
      // page the operator left behind until the next resting redraw.
      d->_refresh.urgent();
      d->_paintDue = true;
    }
    // Drawn when something asked for it, and otherwise at the cadence the page
    // asks for; whether the result reaches the glass is still the policy's
    // answer, and on a panel that costs something to update it usually is not.
    //
    // _paintDue rather than a zeroed timestamp: at a five-minute cadence "last
    // painted at zero" is not "paint now", it is "paint once millis() reaches
    // five minutes" — which on this board meant the boot splash stayed on the
    // glass until somebody pressed the button.
    if (!d->_blank && (d->_paintDue || now - d->_lastPaintMs >= d->cadence())) {
      d->_lastPaintMs = now;
      d->_paintDue = false;
      d->paint();
    }
    });
    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

// What a short press does, whoever delivered it: wake a blanked panel, or
// turn the page and show it now. Three inputs share this — the button, the
// touch layer, and the second button where a board has one — and it is one
// function so they cannot drift apart in what a tap means.
void Display::advancePage(bool forward) {
  if (_blank) { setBlank(false); return; }       // wake only
#if HAS_LVGL_UI
  if (sShellUp) {
    if (LvglUi::idleShowing()) { LvglUi::showIdle(false); return; }  // wake from the clock only
    LvglUi::stepTab(forward ? 1 : -1);           // the buttons walk the tabs
    return;
  }
#endif
  _page = stepPage(_page, forward ? 1 : -1);
  _pageChangedMs = millis();
  _refresh.interval(cadence());          // the page decides how live it is
  _refresh.urgent();                     // and this press is not waiting for it
  _lastPaintMs = _pageChangedMs;
  _paintDue = false;
  paint();
}

// One long press, whoever's button delivered it: the power question on a lit
// shell, a wake on a dark panel, the blank toggle where there is no shell.
// Stated once so the two buttons cannot drift apart in what a squeeze means.
void Display::longPressAction() {
#if HAS_LVGL_UI
  if (sShellUp && !_blank) { LvglUi::showIdle(false); LvglUi::openPowerMenu(); return; }
#endif
  setBlank(!_blank);
}

// Debounced by the poll interval; the grammar lives in PressTracker so the
// three inputs cannot drift apart in what a press means.
void Display::pollButton() {
  const uint32_t now = millis();
  switch (_btn.update(digitalRead(PIN_BUTTON) == LOW, now)) {
    case PressTracker::Event::Press: _lastActivityMs = now; break;
    case PressTracker::Event::Long: longPressAction(); break;
    case PressTracker::Event::Short: advancePage(true); break;
    default: break;
  }
}

// The page after this one. The QR page shows a code for joining the access
// point, and with Wi-Fi off there is no access point to join: a panel that
// still offered it would send someone hunting for a network that is not on
// the air. It is skipped rather than drawn empty.
Display::Page Display::stepPage(Page p, int8_t dir) const {
  Page n = p;
  do { n = (Page)((n + PAGE_COUNT + dir) % PAGE_COUNT); } while (skipPage(n) && n != p);
  return n;
}

// Which pages mean nothing right now. The QR page shows a code for joining
// the access point, and with Wi-Fi off there is no access point to join: a
// panel that still offered it would send someone hunting for a network that
// is not on the air. Stated once, so forward and backward navigation cannot
// disagree about which pages exist.
bool Display::skipPage(Page p) const {
#if !DISPLAY_COMPACT
  if (p == QR && !wifiManager.wifiEnabled()) return true;
#else
  (void)p;
#endif
  return false;
}

#if HAS_TOUCH
// The glass as a button: the same grammar as the physical one, from the same
// tracker. Polled slowly while idle — the controller only answers under a
// finger, and fifty blocking bus transactions a second bought nothing — and
// at the display task's full rate from first contact to release.
void Display::pollTouch() {
  const uint32_t now = millis();
  if (_touch.pressedAt == 0 && now - _lastTouchPollMs < 60) return;
  _lastTouchPollMs = now;
  switch (_touch.update(TouchInput::poll().down, now)) {
    case PressTracker::Event::Press: _lastActivityMs = now; break;
    case PressTracker::Event::Long:  setBlank(!_blank); break;
    case PressTracker::Event::Short: advancePage(true); break;
    default: break;
  }
}
#endif

#if HAS_BUTTON2
// The second button walks the pages backwards. Its long press does nothing on
// purpose — the tracker latches, so a case that squeezes the button for an
// hour neither turns a page on release nor (activity marked only on the press
// edge) holds the backlight on while it does.
void Display::pollButton2() {
  const uint32_t now = millis();
  switch (_btn2.update(digitalRead(PIN_BUTTON2) == LOW, now)) {
    case PressTracker::Event::Press: _lastActivityMs = now; break;
    case PressTracker::Event::Short: advancePage(false); break;
    case PressTracker::Event::Long:
#if HAS_LVGL_UI
      // GPIO35 is the case's power button; the factory firmware put its power
      // menu on this button's long press, so ours does too. A short press
      // still steps back a screen. On mono builds this button's long press
      // stays deliberately silent — the tracker latches, so a case that
      // squeezes it for an hour does nothing.
      longPressAction();
#endif
      break;
    default: break;
  }
}
#endif

void Display::setBlank(bool blank) {
  _blank = blank;
#if HAS_LVGL_UI
  if (sShellUp) LvglUi::onBlank(blank);
#endif
  if (blank) {
    _panel->blank(true);                 // panel + charge pump off
  } else {
    _panel->blank(false);
#if !HAS_LVGL_UI
    // Nothing is known about the glass after it has been off, so the next
    // frame goes out whether or not it matches the last one drawn. Mono
    // boards only: on the shell this painter would smear a half-res page
    // over the LVGL frame — the bench saw it as a flicker at wake — and
    // onBlank() already invalidates the screen for a full repaint.
    _refresh.forget();
    _pageChangedMs = millis();
    _lastPaintMs = _pageChangedMs;
    _paintDue = false;
    paint();
#endif
  }
}

void Display::paint() {
  if (_blank || !_ok) return;
  _panel->clear();
  // One PMU read per frame, shared by the header and whichever page is drawn.
  // The QR page draws neither a header nor battery text, so it skips the four
  // I2C transactions entirely rather than spending them on the panel's bus.
#if DISPLAY_COMPACT
  _bat = Power::battery();
#else
  _bat = (_page == QR) ? Power::Battery{} : Power::battery();
#endif
  // No default: every page is listed, so adding one without drawing it is a
  // build failure rather than a screen that silently shows the status page.
  switch (_page) {
    case STATUS:     paintStatus();    break;
    case NEIGHBORS:  paintNeighbors(); break;
    case TRANSPORT:  paintTransport(); break;
    case RADIO:      paintRadio();     break;
    case NETWORK:    paintNetwork();   break;
#if HAS_GPS
    case GPS:        paintGps();       break;
#endif
  #if !DISPLAY_COMPACT
    case QR:         paintQr();        break;
  #endif
    case PAGE_COUNT: break;                     // not a page
  }
  _chargeSweep = (uint8_t)((_chargeSweep + 6) % 101);   // ~8 s per sweep at 2 fps

  // page indicator, bottom-right — the first thing a small panel drops, since
  // it costs a row of readings to say something a button press already told you
  // No panel enables these today — the page number in the header replaced them
  // — but they were still measuring the screen in constants, so re-enabling
  // them on any other panel would have drawn them off the edge.
  if (DisplayLayout::active().pageDots) {
    const DisplayLayout::Layout dl = DisplayLayout::active();
    const int16_t dy = (int16_t)dl.height - 3;
    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
      const int16_t dx = (int16_t)dl.width - 4 * PAGE_COUNT + 4 * i;
      _gfx->fillRect(dx, dy, 3, 3, i == _page ? _panel->ink() : _panel->paper());
      _gfx->drawRect(dx, dy, 3, 3, _panel->ink());
    }
  }

  // The frame is drawn. Whether it reaches the glass is the panel's question,
  // not the page's: an identical frame is not worth an update, and a panel
  // that costs hundreds of milliseconds is not driven at the rate a page
  // happens to be redrawn at.
  size_t len = 0;
  const uint8_t* fb = _panel->frame(len);
  const uint32_t id = fb ? RefreshPolicy::hash(fb, len) : ++_frameSeq;
  switch (_refresh.decide(millis(), id)) {
    case RefreshPolicy::Action::Skip:    break;
    case RefreshPolicy::Action::Partial: _panel->flush(false); break;
    case RefreshPolicy::Action::Full:    _panel->flush(true);  break;
  }
}

// --- status strip -----------------------------------------------------------
// A battery outline with a nub, filled in six steps that always show the level
// the cell actually holds. While charging, one segment travels up the icon
// inverted against the fill rather than the fill itself moving — which is the
// difference between "there is a cell" and "it is filling", without the
// animation ever claiming the cell is fuller than it is.

// Whether the cell is filling is a question some boards cannot answer at all:
// a divider measures the cell and nothing else, and the charger beside it does
// its work in hardware without telling the processor (Power.h). "Not charging"
// and "no idea" are different readings and the panel has to show the
// difference, or a node plugged in and charging looks exactly like one that is
// quietly draining and somebody goes looking for a fault in a working cable.
// So: "+" while it fills, "?" where the board cannot tell, and nothing at all
// for a cell that is known to be idle — one character, which is all the room
// there is, and written here once because four places used to render
// _bat.charging on its own.
static const char* chargeMark(const Power::Battery& b) {
  if (!b.chargeKnown) return "?";
  return b.charging ? "+" : "";
}

// ... and the same question for the icon, which has only the sweep to say it
// with. A board that cannot tell must not animate: the travelling line is a
// claim, and this one would be made on a field that means nothing.
static bool chargeSure(const Power::Battery& b) { return b.chargeKnown && b.charging; }

// RSSI in dBm to something a bar chart can show. The window is the usable
// range of an SX127x on this band: below the floor a packet is luck, above the
// ceiling the sender is on the desk next to you.
static uint8_t signalPercent(float rssi) {
  const float lo = -135.0f, hi = -75.0f;
  if (rssi <= lo) return 0;
  if (rssi >= hi) return 100;
  return (uint8_t)((rssi - lo) * 100.0f / (hi - lo));
}

// A Wi-Fi link lives in a completely different window: -90 dBm is the edge of
// usable, -40 dBm is the same room. Running it through signalPercent()'s LoRa
// window left every working link pegged at 100%, so the meter could never show
// the uplink getting worse — the one thing it is there to show.
static uint8_t wifiPercent(float rssi) {
  const float lo = -90.0f, hi = -40.0f;
  if (rssi <= lo) return 0;
  if (rssi >= hi) return 100;
  return (uint8_t)((rssi - lo) * 100.0f / (hi - lo));
}

// The header carries the page name and, on the right, the cell — the one
// reading that means the same thing on every page. Signal strength does not
// belong here: a bar chart with no label and no number is decoration, so the
// radio and network pages draw theirs next to the figure they represent.
//
// The title is clipped to whatever space the battery leaves, so a long page
// name can never run into it.
void Display::header(const char* title) {
  // A small panel gives up its title bar first. Ten characters of page name
  // cost a quarter of the panel to say what the readings already imply.
  const DisplayLayout::Layout l = DisplayLayout::active();
  if (!l.header) return;
  _gfx->fillRect(0, 0, l.width, l.headerH, _panel->ink());

  // A status bar, laid out like a phone's: the title hard left, everything else
  // gathered right and assigned right to left so each piece knows where it
  // ends. Slots are reserved whether or not their icon has anything to draw,
  // so nothing shifts sideways when a setting changes.
  const int16_t iy = 1;                       // one pixel of margin under the top
  const uint8_t sz = l.iconSize;
  // Text in this font is seven rows tall inside an eight-row cell, so centring
  // it on the cell height leaves it sitting a row high — which is exactly how
  // the page number looked. Centred on the glyph, with the odd half-pixel given
  // to the top, so it settles level with icons whose weight is at their floor.
  const int16_t ty = (int16_t)(l.headerH - 7 + 1) / 2;
  int16_t right = l.width;

  // The page number sits at the very end, where a paginator belongs.
  char pg[8];
  snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(_page + 1), (unsigned)PAGE_COUNT);
  right -= (int16_t)strlen(pg) * 6;
  _gfx->setTextColor(_panel->paper());
  _gfx->setCursor(right, ty);
  _gfx->print(pg);
  right -= 2;

  // The cell, stood on end: seven pixels wide instead of seventeen.
  if (_bat.present) {
    right -= l.batteryW;
    // Dark on the light header bar, so the travelling line is drawn light.
    DisplayIcons::batteryVertical((*_gfx), right, iy, l.batteryW, l.batteryH,
                                  _bat.percent, chargeSure(_bat), _chargeSweep,
                                  _panel->paper(), _panel->ink());
    right -= 2;
    // A cell that is not animating reads as one that is sitting idle, which on
    // a board with no charger status is a claim it cannot make. Six pixels of
    // title, given up only on the boards that have to say it.
    if (!_bat.chargeKnown) {
      right -= DisplayLayout::FONT_W;
      _gfx->setCursor(right, ty);         // still black-on-white from the page number
      _gfx->print("?");
      right -= 2;
    }
  }

  if (l.statusIcons) {
    // LoRa: bars with an L beside them, because ascending bars alone say
    // something is strong without saying what, and there is a Wi-Fi fan in the
    // same bar that they could just as easily belong to.
    // An odd width, so the columns land exactly two pixels apart: an even one
    // leaves the last of them three from its neighbour and detached from the
    // rest. The height stays the icon size so its floor is level with the cell.
    const uint8_t lw = (uint8_t)(sz % 2 ? sz : sz - 1);
    right -= lw;
    if (g_stats.radioOnline) {
      DisplayIcons::loraSignal((*_gfx), right, iy, lw, sz,
                               g_stats.loraRxPackets ? signalPercent(g_stats.lastRssi) : 0,
                               _panel->paper());
    } else {
      _gfx->setCursor(right + 1, ty); _gfx->print("!");
    }
    right -= 2;

    // Wi-Fi uplink: arcs by strength, a cross when it is configured and down,
    // nothing at all when no uplink has been asked for.
    right -= sz;
    if (wifiManager.stationConfigured()) {
      // One arc at the floor, three at the ceiling: a link that is up has
      // some signal by definition, and rendering it as a bare dot would make a
      // working uplink look like the fault state next to it.
      const uint8_t bars = wifiManager.stationConnected()
          ? (uint8_t)(1 + wifiPercent((float)WiFi.RSSI()) * 2 / 100)
          : DisplayIcons::WIFI_NOT_JOINED;
      DisplayIcons::wifi((*_gfx), right, iy, sz, bars, _panel->paper());
    }
    right -= 2;

    #if HAS_GPS
      right -= sz;
      { Gps::Fix g = Gps::fix();
        if (g.enabled) DisplayIcons::dish((*_gfx), right, iy, sz, g.valid, _panel->paper()); }
      right -= 2;
    #endif
  }

  // Clamped, not merely computed: the cluster grows with optional chrome and on
  // a narrower panel could consume the whole bar. As unsigned arithmetic that
  // wraps to an enormous column count and strlcpy then writes past its buffer.
  const int16_t avail = right - 1;
  const uint8_t columns = avail > 0 ? (uint8_t)(avail / 6) : 0;
  char clipped[22];
  strlcpy(clipped, title, columns + 1 < sizeof(clipped) ? columns + 1 : sizeof(clipped));

  _gfx->setCursor(1, ty);
  _gfx->print(clipped);
  _gfx->setTextColor(_panel->ink());
}

// A 21-column row can carry four counters only if each stays within three
// characters, so anything past 999 is shown in thousands. Saturating at 99k is
// roughly two years of announces at the default interval.
static void compactCount(char* out, size_t n, uint32_t v) {
  if      (v < 1000)   snprintf(out, n, "%lu", (unsigned long)v);
  else if (v < 100000) snprintf(out, n, "%luk", (unsigned long)(v / 1000));
  else                 snprintf(out, n, "99k");
}

// A labelled meter: the text says what is being measured and how much of it
// there is, the bars give the shape at a glance.
void Display::meter(uint8_t row, const char* label, const char* value, uint8_t pct) {
  const int y = DisplayLayout::rowY(row);
  char text[16];
  snprintf(text, sizeof(text), "%-4s%9s", label, value);
  _gfx->setCursor(0, y);
  _gfx->print(text);
  // The same bars the header draws, floor and all, and the same five columns.
  // Thirteen pixels of width gave seven, so the meter beside a figure and the
  // icon in the status bar were reading the same thing on different scales —
  // there is no reason for that, and the precision is in the number anyway.
  DisplayIcons::bars((*_gfx), DisplayLayout::active().width - 10, y, 9, 8,
                     pct, _panel->ink());
}

// 128x64 with the 6x8 built-in font: 21 columns x 8 rows.
void Display::paintStatus() {
  char line[DisplayLayout::rowBytes()];
  uint32_t up = millis() / 1000;

  // Ten columns will not hold "RSSI -104  SNR 11.5", and the left half of that
  // string is worse than either number shown whole. So a small panel gets its
  // own four lines rather than the big page truncated: who this node is, what
  // the radio is doing, what has moved, and for how long.
  if (DisplayLayout::compact()) {
    const char* id = wifiManager.ssid();
    const size_t n = strlen(id);
    _gfx->setCursor(0, DisplayLayout::rowY(0));
    _gfx->print(n > 6 ? id + n - 6 : id);          // the MAC tail identifies it

    _gfx->setCursor(0, DisplayLayout::rowY(1));
    if (g_stats.radioOnline) {
      snprintf(line, sizeof(line), "%.1f S%d",
               (double)settings.radio().freqMhz, settings.radio().sf);
    } else {
      snprintf(line, sizeof(line), "NO RADIO");
    }
    _gfx->print(line);

    // Arrows instead of "R" and "T": two characters back for digits, and a
    // symbol nobody has to be taught. The counts stay — an arrow with no
    // number beside it would say nothing.
    const uint8_t iy = DisplayLayout::rowY(2);
    DisplayIcons::arrow((*_gfx), 0, iy + 1, 7, false, _panel->ink());   // received
    _gfx->setCursor(8, iy);
    snprintf(line, sizeof(line), "%lu", (unsigned long)g_stats.loraRxPackets);
    _gfx->print(line);
    const int16_t tx = 8 + (int16_t)(strlen(line) + 1) * 6;
    DisplayIcons::arrow((*_gfx), tx, iy + 1, 7, true, _panel->ink());   // sent
    _gfx->setCursor(tx + 8, iy);
    snprintf(line, sizeof(line), "%lu", (unsigned long)g_stats.loraTxPackets);
    _gfx->print(line);

    if (_bat.present) snprintf(line, sizeof(line), "%u%%%s %luh%02lum", _bat.percent,
                               chargeMark(_bat),
                               (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
    else              snprintf(line, sizeof(line), "%luh%02lum",
                               (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
    _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print(line);
    return;
  }

  // The node's own tail, not the whole SSID: six characters identify it
  // unambiguously, and the full name is on the network page. The header has
  // icons to carry now.
  { const char* id = wifiManager.ssid(); const size_t n = strlen(id);
    header(n > 6 ? id + n - 6 : id); }

  // Row 1 — portal address / version. The address is the access point's,
  // and with Wi-Fi off there is none to give: say that, rather than print a
  // number nobody can reach.
  //
  // The version takes whatever the address leaves. Since a local build names
  // its commit, FW_VERSION can be "v0.0.9-35-g8465afd" — eight characters more
  // than this row has on a 128x64 panel, and the panel used to cut them off
  // the end, which lands in the middle of the hash and identifies nothing.
  // VersionLabel gives back the longest form that still means something.
  char addr[16];
  // Copied rather than pointed at: toString() returns a temporary, and a
  // pointer into it would dangle before the row below is composed.
  strlcpy(addr, wifiManager.wifiEnabled() ? AP_IP.toString().c_str() : "wifi off", sizeof(addr));
  const size_t used = strlen(addr) + 2;                   // the two spaces between them
  const size_t room = used < DisplayLayout::textWidth() ? DisplayLayout::textWidth() - used : 0;
  char ver[DisplayLayout::rowBytes()];
  VersionLabel::fit(FW_VERSION, room, ver, sizeof(ver));
  _gfx->setCursor(0, DisplayLayout::rowY(0));
  snprintf(line, sizeof(line), "%s%s%s", addr, ver[0] ? "  " : "", ver);
  _gfx->print(line);

  // Row 2 — radio model + channel
  _gfx->setCursor(0, DisplayLayout::rowY(1));
  if (g_stats.radioOnline) {
    snprintf(line, sizeof(line), "%s %.3fM SF%d", g_stats.radioModel,
             (double)settings.radio().freqMhz, settings.radio().sf);
  } else {
    snprintf(line, sizeof(line), "RADIO OFFLINE");
  }
  _gfx->print(line);

  // Row 3 — last RX quality
  _gfx->setCursor(0, DisplayLayout::rowY(2));
  snprintf(line, sizeof(line), "RSSI %4.0f  SNR %5.1f",
           (double)g_stats.lastRssi, (double)g_stats.lastSnr);
  _gfx->print(line);

  // Row 4 — traffic. Arrows carry the direction, which frees the six columns
  // "RX" and "TX" were spending on words the reader learns once. The room goes
  // to the announce counts, which otherwise live only on the transport page —
  // this is the trade the icons exist to make.
  {
    // Counters are rendered in at most three characters, as they are on the
    // transport page, so the row has a worst case rather than a typical one:
    // 8 + 3*6 + 6 twice for the arrows and counts, then nine characters, is
    // 118 of the 128 pixels available. Five-digit counters used to run off the
    // panel entirely.
    const int16_t y = DisplayLayout::rowY(3);
    char n[8];
    DisplayIcons::arrow((*_gfx), 0, y + 1, 7, false, _panel->ink());
    compactCount(n, sizeof(n), g_stats.loraRxPackets);
    _gfx->setCursor(8, y); _gfx->print(n);
    int16_t x = 8 + (int16_t)(strlen(n) + 1) * 6;

    DisplayIcons::arrow((*_gfx), x, y + 1, 7, true, _panel->ink());
    compactCount(n, sizeof(n), g_stats.loraTxPackets);
    _gfx->setCursor(x + 8, y); _gfx->print(n);
    x = x + 8 + (int16_t)(strlen(n) + 1) * 6;

    // The room the two labels used to take now carries the announce count,
    // which otherwise appears only on the transport page.
    char an[8];
    compactCount(an, sizeof(an), g_stats.announcesRx);
    snprintf(line, sizeof(line), "nb%u a%s",
             (unsigned)neighbors.count(NEIGHBOR_STALE_MS), an);
    _gfx->setCursor(x, y); _gfx->print(line);
  }

  // Row 5 — peers + uptime
  _gfx->setCursor(0, DisplayLayout::rowY(4));
  if (_bat.present)
    // The charge marker earns its character: it is how you tell a node that is
    // filling up from one that is quietly draining — or, on a board that cannot
    // see its charger, from one that has no way of knowing which it is doing.
    snprintf(line, sizeof(line), "bat %u%%%s %.2fV %luh%02lum", _bat.percent,
             chargeMark(_bat), (double)_bat.volts,
             (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  else
    snprintf(line, sizeof(line), "rns %u wifi %u  %luh%02lum",
             (unsigned)g_stats.tcpClients, (unsigned)WiFi.softAPgetStationNum(),
             (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  _gfx->print(line);
}

void Display::paintNeighbors() {
  if (DisplayLayout::compact()) {
    Neighbor snap[MAX_NEIGHBORS];
    const size_t n = neighbors.snapshot(snap, MAX_NEIGHBORS);
    const uint32_t now = millis();
    if (n == 0) { _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print("no peers"); return; }
    char l[DisplayLayout::rowBytes()];
    for (size_t i = 0; i < n && i < DisplayLayout::active().rows; i++) {
      const Neighbor& nb = snap[i];
      const uint32_t age = (now - nb.lastSeen) / 1000;
      char ageStr[5];
      if (age < 60)        snprintf(ageStr, sizeof(ageStr), "%lus", (unsigned long)age);
      else if (age < 3600) snprintf(ageStr, sizeof(ageStr), "%lum", (unsigned long)(age / 60));
      else                 snprintf(ageStr, sizeof(ageStr), "%luh", (unsigned long)(age / 3600));
      // Six characters of name and the age: enough to recognise a node you
      // already know, which is all this page is for at this size.
      snprintf(l, sizeof(l), "%-6.6s%3s", nb.name[0] ? nb.name : nb.hash, ageStr);
      _gfx->setCursor(0, DisplayLayout::rowY(i)); _gfx->print(l);
    }
    return;
  }
  Neighbor snap[MAX_NEIGHBORS];
  size_t n = neighbors.snapshot(snap, MAX_NEIGHBORS);
  char line[DisplayLayout::rowBytes()];
  // The list below is the count; the header has icons to carry.
  header("Peers");
  if (n == 0) { _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print("nothing heard yet"); return; }
  uint32_t now = millis();
  for (size_t i = 0; i < n && i < 5; i++) {          // 5 rows fit under the header
    const Neighbor& nb = snap[i];
    const char* name = nb.name[0] ? nb.name : nb.hash;
    char shortName[12]; strlcpy(shortName, name, sizeof(shortName));
    uint32_t age = (now - nb.lastSeen) / 1000;
    char ageStr[8];
    if (age < 60) snprintf(ageStr, sizeof(ageStr), "%lus", (unsigned long)age);
    else if (age < 3600) snprintf(ageStr, sizeof(ageStr), "%lum", (unsigned long)(age / 60));
    else snprintf(ageStr, sizeof(ageStr), "%luh", (unsigned long)(age / 3600));
    if (nb.viaWifi) snprintf(line, sizeof(line), "%-11s wifi %4s", shortName, ageStr);
    else            snprintf(line, sizeof(line), "%-11s %4.0f %4s", shortName, (double)nb.rssi, ageStr);
    _gfx->setCursor(0, DisplayLayout::rowY(i));
    _gfx->print(line);
  }
}

void Display::paintTransport() {
  if (DisplayLayout::compact()) {
    char l[DisplayLayout::rowBytes()];
    const size_t n = RnsTransport::interfaceCount();
    snprintf(l, sizeof(l), "%s", g_stats.transportOnline ? "transport" : "trans OFF");
    _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(l);
    snprintf(l, sizeof(l), "path %u", (unsigned)RnsTransport::pathCount());
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(l);
    snprintf(l, sizeof(l), "if %u", (unsigned)n);
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(l);
    char aRx[6], aTx[6];
    compactCount(aRx, sizeof(aRx), g_stats.announcesRx);
    compactCount(aTx, sizeof(aTx), g_stats.announcesTx);
    snprintf(l, sizeof(l), "an %s/%s", aRx, aTx);
    _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print(l);
    return;
  }
  char line[DisplayLayout::rowBytes()];
  // Four rows fit under the header, so ask for four and take the count
  // separately. A node on a busy LAN has an interface per peer — many more
  // than fit here, and more than a stack buffer on this task wants to hold.
  RnsTransport::IfaceInfo ifs[4];
  // One reading. Asked separately, the count and the rows came from different
  // refreshes, so this panel could print "+2 more" for interfaces that had
  // already gone. The rows are taken now and the remainder worked out from the
  // same total, rather than from whatever the next pass publishes.
  const RnsTransport::Snapshot snap = RnsTransport::snapshot(nullptr, 0, ifs, 4);
  const size_t n = snap.ifaceTotal;
  // "Transport 2 paths" is 17 of the 18 columns the battery leaves, and says
  // what it means without a legend.
  const unsigned paths = (unsigned)snap.pathTotal;
  // The path count moves to the counters row below: the header has icons and a
  // page number to carry now, and nine columns left for a name.
  snprintf(line, sizeof(line), g_stats.transportOnline ? "Transport" : "Trans off");
  header(line);
  // When there are more than fit, the last row counts the remainder instead
  // of letting a live interface vanish.
  const size_t listed = (n > 4) ? min(snap.ifaceRows, (size_t)3) : snap.ifaceRows;
  uint8_t row = 0;
  for (size_t i = 0; i < listed; i++, row++) {
    // Nine columns for a name that carries an address: drop the kind prefix,
    // and for an Auto peer the "fe80::" every link-local starts with, leaving
    // the part that tells one peer from another.
    const char* full = ifs[i].name;
    if (strncmp(full, "WiFi/", 5) == 0)       full += 5;
    else if (strncmp(full, "Auto/", 5) == 0)  full += strncmp(full + 5, "fe80::", 6) == 0 ? 11 : 5;
    char name[12]; strlcpy(name, full, sizeof(name));
    snprintf(line, sizeof(line), "%-9s %-6.6s %3luk", name, ifs[i].mode, (unsigned long)((ifs[i].rxb + ifs[i].txb) / 1024));
    _gfx->setCursor(0, DisplayLayout::rowY(row)); _gfx->print(line);
  }
  if (n > listed) {
    snprintf(line, sizeof(line), "+%u more", (unsigned)(n - listed));
    _gfx->setCursor(0, DisplayLayout::rowY(row)); _gfx->print(line);
  }
  if (n == 0) { _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print("no interfaces"); }

  // Announces and beacons, received/sent. "announce N in N out" ran past the
  // 21 columns the panel has as soon as either counter reached three digits,
  // wrapping under the page dots; and the beacon counters, dropped from the
  // radio page in the redesign, were left with nowhere to live.
  char aRx[6], aTx[6], bRx[6], bTx[6];
  compactCount(aRx, sizeof(aRx), g_stats.announcesRx);
  compactCount(aTx, sizeof(aTx), g_stats.announcesTx);
  compactCount(bRx, sizeof(bRx), g_stats.beaconsRx);
  compactCount(bTx, sizeof(bTx), g_stats.beaconsTx);
  snprintf(line, sizeof(line), "p%u a%s/%s b%s/%s", paths, aRx, aTx, bRx, bTx);
  _gfx->setCursor(0, DisplayLayout::rowY(4)); _gfx->print(line);
}

void Display::paintRadio() {
  if (DisplayLayout::compact()) {
    const RadioSettings& r = settings.radio();
    char l[DisplayLayout::rowBytes()];
    if (!g_stats.radioOnline) { _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print("NO RADIO"); return; }
    snprintf(l, sizeof(l), "%.3f", (double)r.freqMhz);
    _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(l);
    snprintf(l, sizeof(l), "SF%d BW%.0f", r.sf, (double)r.bwKhz);
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(l);
    if (g_stats.loraRxPackets > 0) {
      snprintf(l, sizeof(l), "%.0fdBm", (double)g_stats.lastRssi);
      _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(l);
      snprintf(l, sizeof(l), "%.1fdB", (double)g_stats.lastSnr);
      _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print(l);
    } else {
      _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print("no RX yet");
    }
    return;
  }
  const RadioSettings& r = settings.radio();
  char line[DisplayLayout::rowBytes()];
  header(g_stats.radioOnline ? g_stats.radioModel : "Radio off");
  // Preamble and sync word ride along on the rows that had room for them: a
  // node on the wrong sync word hears nothing, and finding that out should not
  // require the web UI on a device whose whole point is a status panel.
  // No "MHz": the API accepts a preamble up to 1000 symbols and frequencies
  // into four digits, and with the suffix that row reached 24 characters — past
  // both the buffer and the 21 columns, so the preamble it exists to show was
  // the part that got clipped. The unit is on the radio page header's model.
  snprintf(line, sizeof(line), "%.3f %+ddBm p%u", (double)r.freqMhz, r.txDbm, (unsigned)r.preamble);
  _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(line);
  snprintf(line, sizeof(line), "BW%.0f SF%d CR4/%d sy%02X", (double)r.bwKhz, r.sf, r.cr, r.syncWord);
  _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(line);

  // The last packet heard, as a number and as a shape. Nothing received yet
  // means there is nothing to measure, and saying so beats an empty chart.
  if (g_stats.loraRxPackets > 0) {
    snprintf(line, sizeof(line), "%.0fdBm", (double)g_stats.lastRssi);
    meter(2, "sig", line, signalPercent(g_stats.lastRssi));
    snprintf(line, sizeof(line), "%.1fdB", (double)g_stats.lastSnr);
    meter(3, "snr", line, loraQualityPercent(g_stats.lastSnr, r.sf));
  } else {
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print("sig  no RX yet");
  }

  if (g_stats.dutyLocked)
    snprintf(line, sizeof(line), "duty FULL %us", (unsigned)g_stats.dutyRetryS);
  else if (g_stats.dutyLimitBp)
    snprintf(line, sizeof(line), "duty %.2f/%.2f%% cw%u", (double)(g_stats.airtimeLong * 100.0f),
             g_stats.dutyLimitBp / 100.0f, g_stats.csmaBand);
  else
    // The contention band belongs here too: it is set from channel use whether
    // or not a duty limit is being enforced.
    snprintf(line, sizeof(line), "air %.2f%% nolim cw%u", (double)(g_stats.airtimeLong * 100.0f),
             g_stats.csmaBand);
  _gfx->setCursor(0, DisplayLayout::rowY(4)); _gfx->print(line);
}

#if HAS_GPS
// What the receiver can see. Before a fix the satellite count is the useful
// number — it is what tells you whether the antenna has a view of the sky.
void Display::paintGps() {
  Gps::Fix g = Gps::fix();
  char line[DisplayLayout::rowBytes()];
  header(g.enabled ? (g.valid ? "GNSS fix" : "GNSS scan") : "GNSS off");
  if (!g.enabled) {
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print("Receiver powered down");
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print("Enable on the settings");
    _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print("page.");
    return;
  }
  snprintf(line, sizeof(line), "sats %u  hdop %.1f", g.satellites, (double)g.hdop);
  _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(line);
  if (g.valid) {
    snprintf(line, sizeof(line), "%+.5f", g.latitude);   _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(line);
    snprintf(line, sizeof(line), "%+.5f", g.longitude);  _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(line);
    snprintf(line, sizeof(line), "alt %.0fm  %.0fkm/h", (double)g.altitude, (double)g.speedKmh);
    _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print(line);
  } else {
    snprintf(line, sizeof(line), "%lu sentences", (unsigned long)g.sentences);
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(line);
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(g.sentences ? "waiting for a fix" : "no data from module");
  }
  if (g.timeValid) { _gfx->setCursor(0, DisplayLayout::rowY(4)); _gfx->print(g.utc + 11); _gfx->print(g.clockSet ? " UTC sync" : " UTC"); }
  else             { _gfx->setCursor(0, DisplayLayout::rowY(4)); _gfx->print("no time yet"); }
}
#endif

// Scan-to-join. The panel is 128x64 and a version-3 code is 29 modules, so
// the code takes the left 62 px at two pixels per module (with a one-module
// quiet zone) and the text goes beside it. Lit pixels are the light modules:
// a phone camera reads the OLED like ink on paper.
#if !DISPLAY_COMPACT
void Display::paintQr() {
  char text[192];
  bool open = settings.wifi().security == ApSecurity::Open;
  if (!Qr::payloadText(Qr::Payload::Wifi, text, sizeof(text))) { _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print("QR: payload too long"); return; }

  // Rebuilding costs a few milliseconds, so only do it when it changed.
  static char builtFor[192] = "";
  static uint8_t buffer[Qr::MAX_BUFFER];
  static QRCode qr;
  static bool ok = false;
  if (strcmp(builtFor, text) != 0) {
    ok = Qr::encode(text, qr, buffer);
    strlcpy(builtFor, text, sizeof(builtFor));
  }
  if (!ok) { _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print("QR encode failed"); return; }

  // The code gets up to half the width and as much height as there is, from
  // the layout rather than from the 62 and 64 that were this page's memory of
  // one panel: on anything wider than 128 those literals put the symbol in a
  // corner and centred it against the wrong height. Three is as large as a
  // module is allowed to get — beyond that a bigger panel buys resolution the
  // phone did not need and space the captions did.
  const DisplayLayout::Layout ql = DisplayLayout::active();
  const int16_t budget = min((int16_t)(ql.width / 2), (int16_t)ql.height);
  const uint8_t scale = (uint8_t)max(1, min(3, budget / (qr.size + 2)));
  const uint8_t span  = (uint8_t)((qr.size + 2) * scale);     // + 1 module quiet zone
  const uint8_t x0 = 0, y0 = (uint8_t)((ql.height - span) / 2);
  _gfx->fillRect(x0, y0, span, span, _panel->ink());
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        _gfx->fillRect(x0 + (x + 1) * scale, y0 + (y + 1) * scale, scale, scale, _panel->paper());

  const uint8_t tx = (uint8_t)(span + 3);
  _gfx->setCursor(tx, 4);  _gfx->print("Scan to");
  _gfx->setCursor(tx, 13); _gfx->print("join wifi");
  const char* ssid = wifiManager.ssid();
  size_t len = strlen(ssid);
  char line[12];
  strlcpy(line, ssid, sizeof(line));
  _gfx->setCursor(tx, 26); _gfx->print(line);
  if (len >= sizeof(line)) { _gfx->setCursor(tx, 35); _gfx->print(ssid + sizeof(line) - 1); }
  _gfx->setCursor(tx, 48); _gfx->print(open ? "open" : "WPA2");
}
#endif  // !DISPLAY_COMPACT

void Display::paintNetwork() {
  if (DisplayLayout::compact()) {
    char l[DisplayLayout::rowBytes()];
    const char* id = wifiManager.ssid();
    const size_t idn = strlen(id);
    _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(idn > 10 ? id + idn - 10 : id);
    if (!wifiManager.wifiEnabled()) {
      _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print("wifi off");
      snprintf(l, sizeof(l), "rns%u", (unsigned)g_stats.tcpClients);
      _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(l);
      _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print("console:ON");
      return;
    }
    snprintf(l, sizeof(l), "ch%u %s", settings.wifi().channel, wifiManager.securityName());
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(l);
    snprintf(l, sizeof(l), "cli%u rns%u", (unsigned)WiFi.softAPgetStationNum(),
             (unsigned)g_stats.tcpClients);
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print(l);
    _gfx->setCursor(0, DisplayLayout::rowY(3));
    _gfx->print((wifiManager.stationConnected() ? WiFi.localIP() : AP_IP).toString().c_str());
    return;
  }
  char line[DisplayLayout::rowBytes()];
  header("Network");
  // The AP address is the fixed 10.42.0.1 and already sits on the status page,
  // so the row it used to occupy carries the channel and the two counts that
  // say whether anyone is actually using the node. Those used to be hidden the
  // moment the node joined a network — which is exactly the deployment where
  // you would go looking for them.
  // 12 for the name, not 15: the longest security label is "wpa2wpa3" at eight
  // characters, and 15 + 1 + 8 overruns the 21 columns — clipping away the
  // security this row is here to report.
  if (!wifiManager.wifiEnabled()) {
    // No access point and no station: the panel says so in the rows that
    // would otherwise describe a network that is not on the air, and names
    // the way back, since the console is then the only one.
    _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print("Wi-Fi off");
    snprintf(line, sizeof(line), "rns %u  console on", (unsigned)g_stats.tcpClients);
    _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(line);
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print("up   WIFI ON @console");
  } else {
  snprintf(line, sizeof(line), "%-12.12s %s", wifiManager.ssid(), wifiManager.securityName());
  _gfx->setCursor(0, DisplayLayout::rowY(0)); _gfx->print(line);
  snprintf(line, sizeof(line), "ch%-2u cli %u  rns %u", settings.wifi().channel,
           (unsigned)WiFi.softAPgetStationNum(), (unsigned)g_stats.tcpClients);
  _gfx->setCursor(0, DisplayLayout::rowY(1)); _gfx->print(line);

  // Joined a network? Then there is one link worth measuring, and it gets a
  // label and a number like everything else.
  if (wifiManager.stationConnected()) {
    // One sample for both the figure and the bars: the driver re-reads RSSI on
    // every call and it moves several dB, so two reads disagree on one row.
    const int rssi = WiFi.RSSI();
    snprintf(line, sizeof(line), "%ddBm", rssi);
    meter(2, "up", line, wifiPercent((float)rssi));
  } else {
    _gfx->setCursor(0, DisplayLayout::rowY(2)); _gfx->print("up   not joined");
  }
  }
  // The delivery address: the row exists so somebody standing at the node can
  // read off where to reach it, and the retimesh.node destination it used to
  // show is announced by nobody and answered by nothing.
  _gfx->setCursor(0, DisplayLayout::rowY(3)); _gfx->print("addr ");
  _gfx->print(String(nodeIdentity.lxmfHex()).substring(0, 16));
#if HAS_SD
  SdCard::Info si = sdCard.info();
  if (si.state == SdCard::State::Absent) snprintf(line, sizeof(line), "SD: none");
  else snprintf(line, sizeof(line), "SD: %.0fG %s", si.cardBytes / 1e9, SdCard::stateName(si.state));
#else
  if (_bat.present) snprintf(line, sizeof(line), "batt %.2fV %u%%%s", (double)_bat.volts, _bat.percent, chargeMark(_bat));
  else              snprintf(line, sizeof(line), "USB power");
#endif
  _gfx->setCursor(0, DisplayLayout::rowY(4)); _gfx->print(line);
}

#else
// A headless board: the interface stays, so no caller needs a guard of its
// own, and nothing behind it is compiled — the panel driver, the pages, the
// icons and the QR page all stay out of the image. main.cpp creates no
// display task either.
bool Display::begin() { return false; }
void Display::notice(const char*, const char*) {}
#endif
