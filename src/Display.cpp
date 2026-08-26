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
#include <WiFi.h>
#include "WifiManager.h"

Display display;

bool Display::begin() {
#if HAS_DISPLAY
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  // periphBegin=false: Wire is already up on the board-specific pins.
  if (!_oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    log_w("No SSD1306 at 0x%02X (SDA %d / SCL %d) — display disabled",
          OLED_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
    return false;
  }
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
  for (;;) {
    d->paint();
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));
  }
}

// 128x64 with the 6x8 built-in font: 21 columns x 8 rows.
void Display::paint() {
#if HAS_DISPLAY
  char line[24];
  uint32_t up = millis() / 1000;

  _oled.clearDisplay();

  // Row 0 — SSID, inverted as a header bar
  _oled.fillRect(0, 0, 128, 9, SSD1306_WHITE);
  _oled.setTextColor(SSD1306_BLACK);
  _oled.setCursor(1, 1);
  _oled.print(wifiManager.ssid());
  _oled.setTextColor(SSD1306_WHITE);

  // Row 1 — portal address / version
  _oled.setCursor(0, 12);
  snprintf(line, sizeof(line), "10.42.0.1  %s", FW_VERSION);
  _oled.print(line);

  // Row 2 — radio model + channel
  _oled.setCursor(0, 22);
  if (g_stats.radioOnline) {
    snprintf(line, sizeof(line), "%s %.3fM SF%d", g_stats.radioModel,
             (double)RF_FREQ_MHZ, RF_SF);
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
  snprintf(line, sizeof(line), "RX %-6lu TX %-6lu",
           (unsigned long)g_stats.loraRxPackets, (unsigned long)g_stats.loraTxPackets);
  _oled.print(line);

  // Row 5 — peers + uptime
  _oled.setCursor(0, 52);
  snprintf(line, sizeof(line), "rns %u wifi %u  %luh%02lum",
           (unsigned)g_stats.tcpClients, (unsigned)WiFi.softAPgetStationNum(),
           (unsigned long)(up / 3600), (unsigned long)(up % 3600 / 60));
  _oled.print(line);

  _oled.display();
#endif
}
