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
//  WifiManager.h — SoftAP, captive portal DNS, and the port-80 web app
//
//  Runs entirely on core 0 (next to the ESP32 Wi-Fi/LwIP stack):
//    - SoftAP "retimesh-XXXXXX" (prefix + last 3 MAC octets) at 10.42.0.1
//    - DNSServer answering every A query with 10.42.0.1 (captive portal);
//      polled from a small task pinned to core 0
//    - AsyncWebServer on port 80:
//        /               single-page app from LittleFS (data/index.html)
//        /api/status     JSON node stats (uptime, RSSI/SNR, peers, ...)
//        /api/board      GET list / POST new post — public bulletin board,
//                        persisted to LittleFS, deliberately unencrypted
//        (unknown host)  302 -> portal, which triggers the OS sign-in UI
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include "Config.h"

class WifiManager {
public:
  // Brings up the AP, DNS and HTTP server. LittleFS must be mounted first.
  void begin();

  // The SSID actually in use (derived from the MAC unless AP_SSID is set).
  const char* ssid() const { return _ssid; }

  // FreeRTOS entry point — created pinned to core 0 from main.cpp.
  // DNSServer has no async mode; it needs a polling loop.
  static void dnsTask(void* self);

private:
  void setupRoutes();
  void handleStatus(AsyncWebServerRequest* request);
  void handleBoardGet(AsyncWebServerRequest* request);
  void handleBoardPost(AsyncWebServerRequest* request,
                       const uint8_t* data, size_t len,
                       size_t index, size_t total);

  DNSServer       _dns;
  AsyncWebServer  _http{HTTP_PORT};
  char            _ssid[33] = {0};       // 32 chars max + NUL
};

extern WifiManager wifiManager;
