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
#include "Display.h"
#include "QrCode.h"
#include <WiFi.h>
#include "WifiManager.h"
#include "Settings.h"
#include "Neighbors.h"
#include "RnsAnnounce.h"
#include "SdCard.h"
#include "RnsTransport.h"
#include "Power.h"

Display display;

bool Display::ack(uint8_t addr) {
#if HAS_DISPLAY
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;    // 0 = ACK received
#else
  return false;
#endif
}

bool Display::begin() {
#if HAS_DISPLAY
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.setTimeOut(50);                   // a missing panel must not stall boot

  const uint8_t candidates[] = { OLED_ADDR, (uint8_t)(OLED_ADDR == 0x3C ? 0x3D : 0x3C) };
  for (uint8_t a : candidates) {
    if (ack(a)) { _addr = a; break; }
  }
  if (_addr == 0) {
    log_w("No I2C device at 0x%02X/0x%02X (SDA %d / SCL %d) — display disabled",
          candidates[0], candidates[1], PIN_OLED_SDA, PIN_OLED_SCL);
    return false;
  }

  // periphBegin=false: Wire is already up on the board-specific pins.
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, _addr, true, false)) {
    log_w("SSD1306 at 0x%02X did not initialise — display disabled", _addr);
    return false;
  }
  log_i("SSD1306 found at 0x%02X (SDA %d / SCL %d)", _addr, PIN_OLED_SDA, PIN_OLED_SCL);
  _oled.setRotation(OLED_ROTATION);
  _oled.clearDisplay();
  _oled.setTextColor(SSD1306_WHITE);
  _oled.setTextSize(1);
  _oled.setCursor(0, 0);
  _oled.print(FW_NAME);
  _oled.setCursor(0, 12);
  _oled.print("booting...");
  _oled.display();
  _ok = true;
#endif
  return _ok;
}

void Display::displayTask(void* self) {
  auto* d = static_cast<Display*>(self);
  if (!d->_ok) { vTaskDelete(nullptr); return; }
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  uint32_t lastPaint = 0;
  d->_lastActivityMs = millis();
  for (;;) {
    d->pollButton();
    uint32_t now = millis();
    if (d->_page != STATUS && now - d->_pageChangedMs > DISPLAY_PAGE_TIMEOUT_MS) {
      d->_page = STATUS; d->_pageChangedMs = now; lastPaint = 0;
    }
    // Battery saving: switch the panel off after a minute without a press.
    if (!d->_blank && now - d->_lastActivityMs > Power::displaySleepMs()) d->setBlank(true);
    if (!d->_blank && now - lastPaint >= DISPLAY_REFRESH_MS) { lastPaint = now; d->paint(); }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
  }
}

// Debounced by the poll interval: a press is registered on release (short)
// or once the long threshold passes while still held (long).
void Display::pollButton() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && _pressedAtMs == 0) { _pressedAtMs = now; _longFired = false; _lastActivityMs = now; return; }
  if (down && !_longFired && now - _pressedAtMs >= BUTTON_LONG_MS) {
    _longFired = true;
    setBlank(!_blank);
    return;
  }
  if (!down && _pressedAtMs != 0) {
    if (!_longFired && now - _pressedAtMs >= 30) {          // short press
      if (_blank) setBlank(false);                           // wake only
      else { _page = (Page)((_page + 1) % PAGE_COUNT); _pageChangedMs = now; paint(); }
    }
    _pressedAtMs = 0;
  }
}

void Display::setBlank(bool blank) {
  _blank = blank;
#if HAS_DISPLAY
  if (blank) {
    _oled.ssd1306_command(SSD1306_DISPLAYOFF);           // panel + charge pump off
  } else {
    _oled.ssd1306_command(SSD1306_DISPLAYON);
    _pageChangedMs = millis();
    paint();
  }
#endif
}

void Display::paint() {
#if HAS_DISPLAY
  if (_blank) return;
  _oled.clearDisplay();
  // No default: every page is listed, so adding one without drawing it is a
  // build failure rather than a screen that silently shows the status page.
  switch (_page) {
    case STATUS:     paintStatus();    break;
    case NEIGHBORS:  paintNeighbors(); break;
    case TRANSPORT:  paintTransport(); break;
    case RADIO:      paintRadio();     break;
    case NETWORK:    paintNetwork();   break;
    case QR:         paintQr();        break;
    case PAGE_COUNT: break;                     // not a page
  }
  // page indicator, bottom-right
  for (uint8_t i = 0; i < PAGE_COUNT; i++)
    _oled.fillRect(128 - 4 * PAGE_COUNT + 4 * i, 61, 3, 3, i == _page ? SSD1306_WHITE : SSD1306_BLACK),
    _oled.drawRect(128 - 4 * PAGE_COUNT + 4 * i, 61, 3, 3, SSD1306_WHITE);
  _oled.display();
#endif
}

#if HAS_DISPLAY
static void header(Adafruit_SSD1306& o, const char* text) {
  o.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  o.setTextColor(SSD1306_BLACK);
  o.setCursor(1, 1);
  o.print(text);
  o.setTextColor(SSD1306_WHITE);
}
#endif

// 128x64 with the 6x8 built-in font: 21 columns x 8 rows.
void Display::paintStatus() {
#if HAS_DISPLAY
  char line[24];
  uint32_t up = millis() / 1000;

  header(_oled, wifiManager.ssid());

  // Row 1 — portal address / version
  _oled.setCursor(0, 12);
  snprintf(line, sizeof(line), "10.42.0.1  %s", FW_VERSION);
  _oled.print(line);

  // Row 2 — radio model + channel
  _oled.setCursor(0, 22);
  if (g_stats.radioOnline) {
    snprintf(line, sizeof(line), "%s %.3fM SF%d", g_stats.radioModel,
             (double)settings.radio().freqMhz, settings.radio().sf);
  } else {
    snprintf(line, sizeof(line), "RADIO OFFLINE");
  }
  _oled.print(line);

  // Row 3 — last RX quality
  _oled.setCursor(0, 32);
  snprintf(line, sizeof(line), "RSSI %4.0f  SNR %5.1f",
           (double)g_stats.lastRssi, (double)g_stats.lastSnr);
  _oled.print(line);

  // Row 4 — traffic
  _oled.setCursor(0, 42);
  snprintf(line, sizeof(line), "RX %-5lu TX %-5lu NB %u",
           (unsigned long)g_stats.loraRxPackets, (unsigned long)g_stats.loraTxPackets,
           (unsigned)neighbors.count(NEIGHBOR_STALE_MS));
  _oled.print(line);

  // Row 5 — peers + uptime
  _oled.setCursor(0, 52);
  Power::Battery bat = Power::battery();
  if (bat.present)
    snprintf(line, sizeof(line), "bat %u%% %.2fV  %luh%02lum", bat.percent, (double)bat.volts,
             (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  else
    snprintf(line, sizeof(line), "rns %u wifi %u  %luh%02lum",
             (unsigned)g_stats.tcpClients, (unsigned)WiFi.softAPgetStationNum(),
             (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  _oled.print(line);
#endif
}

void Display::paintNeighbors() {
#if HAS_DISPLAY
  Neighbor snap[MAX_NEIGHBORS];
  size_t n = neighbors.snapshot(snap, MAX_NEIGHBORS);
  char line[24];
  snprintf(line, sizeof(line), "Neighbours: %u", (unsigned)n);
  header(_oled, line);
  if (n == 0) { _oled.setCursor(0, 14); _oled.print("nothing heard yet"); return; }
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
    _oled.setCursor(0, 12 + i * 10);
    _oled.print(line);
  }
#endif
}

void Display::paintTransport() {
#if HAS_DISPLAY
  char line[24];
  RnsTransport::IfaceInfo ifs[RNS_MAX_CLIENTS + 1];
  size_t n = RnsTransport::interfaces(ifs, RNS_MAX_CLIENTS + 1);
  snprintf(line, sizeof(line), "Transport %s %u path%s",
           g_stats.transportOnline ? "up" : "OFF", (unsigned)RnsTransport::pathCount(),
           RnsTransport::pathCount() == 1 ? "" : "s");
  header(_oled, line);
  uint8_t row = 0;
  for (size_t i = 0; i < n && row < 5; i++, row++) {
    char name[12]; strlcpy(name, ifs[i].name, sizeof(name));
    if (strncmp(name, "WiFi/", 5) == 0) memmove(name, name + 5, strlen(name + 5) + 1);   // just the IP
    snprintf(line, sizeof(line), "%-9s %-6.6s %3luk", name, ifs[i].mode, (unsigned long)((ifs[i].rxb + ifs[i].txb) / 1024));
    _oled.setCursor(0, 12 + row * 10); _oled.print(line);
  }
  if (n == 0) { _oled.setCursor(0, 14); _oled.print("no interfaces"); }
#endif
}

void Display::paintRadio() {
#if HAS_DISPLAY
  const RadioSettings& r = settings.radio();
  char line[24];
  header(_oled, g_stats.radioOnline ? g_stats.radioModel : "Radio OFFLINE");
  snprintf(line, sizeof(line), "%.3f MHz %+d dBm", (double)r.freqMhz, r.txDbm);       _oled.setCursor(0, 12); _oled.print(line);
  snprintf(line, sizeof(line), "BW %.1fk SF%d CR4/%d", (double)r.bwKhz, r.sf, r.cr); _oled.setCursor(0, 22); _oled.print(line);
  snprintf(line, sizeof(line), "sync %02X pre %u", r.syncWord, r.preamble);            _oled.setCursor(0, 32); _oled.print(line);
  if (g_stats.dutyLocked)
    snprintf(line, sizeof(line), "duty FULL %us", (unsigned)g_stats.dutyRetryS);
  else if (r.dutyCyclePct)
    snprintf(line, sizeof(line), "duty %.2f/%u%% cw%u", (double)(g_stats.airtimeLong * 100.0f), r.dutyCyclePct, g_stats.csmaBand);
  else
    snprintf(line, sizeof(line), "air %.2f%% cw%u", (double)(g_stats.airtimeLong * 100.0f), g_stats.csmaBand);
  _oled.setCursor(0, 42); _oled.print(line);
  snprintf(line, sizeof(line), "an %lu/%lu bc %lu/%lu", (unsigned long)g_stats.announcesRx, (unsigned long)g_stats.announcesTx,
           (unsigned long)g_stats.beaconsRx, (unsigned long)g_stats.beaconsTx);        _oled.setCursor(0, 52); _oled.print(line);
#endif
}

// Scan-to-join. The panel is 128x64 and a version-3 code is 29 modules, so
// the code takes the left 62 px at two pixels per module (with a one-module
// quiet zone) and the text goes beside it. Lit pixels are the light modules:
// a phone camera reads the OLED like ink on paper.
void Display::paintQr() {
#if HAS_DISPLAY
  char text[192];
  bool open = settings.wifi().security == ApSecurity::Open;
  if (!Qr::payloadText(Qr::Payload::Wifi, text, sizeof(text))) { _oled.setCursor(0, 24); _oled.print("QR: payload too long"); return; }

  // Rebuilding costs a few milliseconds, so only do it when it changed.
  static char builtFor[192] = "";
  static uint8_t buffer[Qr::MAX_BUFFER];
  static QRCode qr;
  static bool ok = false;
  if (strcmp(builtFor, text) != 0) {
    ok = Qr::encode(text, qr, buffer);
    strlcpy(builtFor, text, sizeof(builtFor));
  }
  if (!ok) { _oled.setCursor(0, 24); _oled.print("QR encode failed"); return; }

  const uint8_t scale = (uint8_t)max(1, min(2, 62 / (qr.size + 2)));
  const uint8_t span  = (uint8_t)((qr.size + 2) * scale);     // + 1 module quiet zone
  const uint8_t x0 = 0, y0 = (uint8_t)((64 - span) / 2);
  _oled.fillRect(x0, y0, span, span, SSD1306_WHITE);
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        _oled.fillRect(x0 + (x + 1) * scale, y0 + (y + 1) * scale, scale, scale, SSD1306_BLACK);

  const uint8_t tx = (uint8_t)(span + 3);
  _oled.setCursor(tx, 4);  _oled.print("Scan to");
  _oled.setCursor(tx, 13); _oled.print("join wifi");
  const char* ssid = wifiManager.ssid();
  size_t len = strlen(ssid);
  char line[12];
  strlcpy(line, ssid, sizeof(line));
  _oled.setCursor(tx, 26); _oled.print(line);
  if (len >= sizeof(line)) { _oled.setCursor(tx, 35); _oled.print(ssid + sizeof(line) - 1); }
  _oled.setCursor(tx, 48); _oled.print(open ? "open" : "WPA2");
#endif
}

void Display::paintNetwork() {
#if HAS_DISPLAY
  char line[24];
  header(_oled, "Network");
  snprintf(line, sizeof(line), "%s", wifiManager.ssid());                                  _oled.setCursor(0, 12); _oled.print(line);
  snprintf(line, sizeof(line), "%s  ch%u  %s", WiFi.softAPIP().toString().c_str(),
           settings.wifi().channel, wifiManager.securityName());                           _oled.setCursor(0, 22); _oled.print(line);
  snprintf(line, sizeof(line), "wifi %u  rns tcp %u", (unsigned)WiFi.softAPgetStationNum(),
           (unsigned)g_stats.tcpClients);                                                  _oled.setCursor(0, 32); _oled.print(line);
  _oled.setCursor(0, 42); _oled.print("dest ");
  _oled.print(String(nodeIdentity.destHex()).substring(0, 16));
  SdCard::Info si = sdCard.info();
  if (si.state == SdCard::State::Absent) snprintf(line, sizeof(line), "SD: none");
  else snprintf(line, sizeof(line), "SD: %.0fG %s", si.cardBytes / 1e9, SdCard::stateName(si.state));
  _oled.setCursor(0, 52); _oled.print(line);
#endif
}
