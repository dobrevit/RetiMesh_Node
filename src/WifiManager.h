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
//    - SoftAP "retimesh-XXXXXX" (prefix + last 3 MAC octets, or a custom
//      SSID from settings) at 10.42.0.1 — open, WPA2, WPA2/WPA3 or WPA3
//    - a resolver bound to 10.42.0.1 answering every A query with it (captive portal);
//      polled from a small task pinned to core 0
//    - AsyncWebServer on port 80:
//        /                 single-page app from LittleFS (data/index.html)
//        /api/status       JSON node stats (uptime, RSSI/SNR, peers, ...)
//        /api/board        GET list / POST new post — public bulletin board
//        /settings.html    admin page (HTTP Basic Auth, user "admin")
//        /api/settings     GET all / POST radio|wifi|admin|reset (auth)
//        /api/system/*     bootloader and reboot (auth, local links only)
//        (unknown host)    302 -> portal, which triggers the OS sign-in UI
//
//  The HTTP server binds 0.0.0.0, so it answers on every lwIP interface —
//  the access point and station today, USB or PPP links when a build carries
//  them — and with Wi-Fi switched off (settings.links) it still starts, for
//  whichever of those is there. Wi-Fi is one local link among several; see
//  LocalLink.h.
//
//  Radio changes apply live through LoRaRadio::requestReconfigure();
//  Wi-Fi changes are saved and followed by a scheduled restart, because
//  reconfiguring the AP drops the very connection the request came on.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "CaptiveDns.h"
#include <ESPAsyncWebServer.h>
#include "Config.h"
#include "Settings.h"
#include "QrCode.h"

class WifiManager {
public:
  // Brings up the AP, DNS and HTTP server. LittleFS + Settings must be
  // ready first.
  void begin();

  // The SSID actually in use (derived from the MAC unless configured).
  const char* ssid() const { return _ssid; }
  // The mDNS name this node answers to, without the ".local" suffix. Derived
  // from the access-point name so there is one identity per node rather than
  // two that can disagree.
  const char* hostname() const { return _hostname; }
  void resolveNames();                   // ssid + hostname, without starting anything
  bool dnsListening() { return _dns.listening(); }
  bool stationConfigured() const { return settings.wifi().staSsid[0] != '\0'; }
  bool stationConnected() const { return WiFi.status() == WL_CONNECTED; }
  const char* securityName() const { return _securityName; }

  // Called from loop(): station watchdog. Restarts go through Bootloader
  // (Bootloader.h), which answers whether it will honour one.
  void tick();
  bool wifiEnabled() const;

private:
  void startAccessPoint();
  void setupRoutes();
  bool authed(AsyncWebServerRequest* request);

  void handleStatus(AsyncWebServerRequest* request);
  void handleBoardGet(AsyncWebServerRequest* request);
  void handleQrFor(AsyncWebServerRequest* request, Qr::Payload what);
  void handleBoardPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleSettingsGet(AsyncWebServerRequest* request);
  void handleRadioPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleWifiPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleAdminPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleTransportPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleSdFormat(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleSdAdopt(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleSdEject(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleExport(AsyncWebServerRequest* request);
  void handleImport(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleReset(AsyncWebServerRequest* request);
  void handleLinksPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleMaintenancePost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleBootloaderGet(AsyncWebServerRequest* request);
  void handleBootloaderPost(AsyncWebServerRequest* request, const char* body, size_t len);
  void handleRebootPost(AsyncWebServerRequest* request, const char* body, size_t len);

  CaptiveDns      _dns;                  // the access point's resolver, bound to its address
  AsyncWebServer  _http{HTTP_PORT};
  char            _ssid[33] = {0};       // 32 chars max + NUL
  const char*     _securityName = "open";
  char            _hostname[33] = "";
  void            deriveHostname();      // _ssid -> a legal mDNS label
  // Read once from /assets.json at begin(). "" when the file is absent, which
  // means a filesystem written before stamping existed.
  String          _assetStamp;
  uint32_t        _staRetryAt = 0;
};

extern WifiManager wifiManager;
