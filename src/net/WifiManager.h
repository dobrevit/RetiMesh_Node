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
//    - a resolver answering every A query that arrives at 10.42.0.1 with it
//      (captive portal), from AsyncUDP's callback; no task of its own
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

  // Re-applies wifi.sta_listen_interval to the station config, by
  // read-modify-write — WiFi.begin() builds that config from scratch, so
  // every begin() call site and the STA_START hook call this after it, and
  // the settings commit calls it for a live change. Public for the commit's
  // sake (SettingsFields::commitWifi); safe to call with no station up (it
  // does nothing then).
  void applyStaListenInterval();

  // --- joining a network from the glass -----------------------------------
  // The GUI's scanner drives these. A live join, deliberately: the AP's
  // settings apply at a restart because the AP cannot be rebuilt under the
  // request that changed it, but a station join applies itself — so success
  // is proven on air first and only then written down, with no restart.
  struct StaScanEntry {
    char   ssid[33];
    int8_t rssi;
    bool   secured;
  };
  enum class StaJoin : uint8_t { Idle, Trying, Joined, Failed };
  void staScanStart();                   // a request; tick() starts the scan on the loop task
  int  staScanCount();                   // -1 still scanning (or not started yet), else how many
  bool staScanResult(int i, StaScanEntry& out);
  void staScanDone();                    // frees the result table; tick() re-asserts the mode
  bool staJoin(const char* ssid, const char* password);  // false: unusable credentials; tick() starts it
  StaJoin staJoinState();                // pure one-shot verdict; tick() does the work
  void staForget();                      // disconnect and clear the stored network
  int  staRssi() const;                  // dBm while connected, 0 otherwise
  void staIpText(char* out, size_t n) const;

private:
  void startAccessPoint();
  // The radio shape the settings ask for — the AP and STA switches, and
  // whether a station is even configured, combined exactly once.
  // startAccessPoint() brings the node up in this shape and tick()'s
  // join-failure path falls back to it, so the rule must not exist twice.
  wifi_mode_t settingsWifiMode(bool& wantAp, bool& wantSta) const;
  void setupRoutes();
  bool authed(AsyncWebServerRequest* request);

  void handleStatus(AsyncWebServerRequest* request);
  void handleMessages(AsyncWebServerRequest* request);
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
  // The join in progress, if any. Volatile: the GUI polls from the display
  // task while tick() runs on the loop task.
  volatile bool   _joining = false;
  volatile uint8_t _joinVerdict = 0;     // 0 none, 1 joined, 2 failed — one-shot
  char            _joinSsid[33] = "";
  char            _joinPass[65] = "";
  uint32_t        _joinDeadline = 0;
  // Every WiFi driver mutation after boot — mode(), begin(), scanNetworks()
  // — is tick()'s, on the loop task; the display task only asks. These flags
  // are the asking — volatile, single-slot hand-offs like _joining/
  // _joinVerdict above, state put down before the flag is raised:
  //   _scanReq / _joinReq — the glass asked for a scan / a join (credentials
  //   already in _joinSsid/_joinPass). tick() serves them before it touches
  //   the mode, so a request pending is a request that will start on the
  //   settled shape — the old design let the display task call
  //   scanNetworks()/begin() itself, and tick() could observe "nothing in
  //   flight" and strip, with WiFi.mode(), the STA bit the display task was
  //   raising at that very moment. staScanDone()/staForget() clear an
  //   unserved request: an ask abandoned at the glass is cancelled, not left
  //   to start work nobody will collect.
  //   _scanActive — the scan tick() started for the glass is still out.
  //   tick() must not touch the mode under it: the core raised the STA bit
  //   itself inside scanNetworks(), and stripping the bit aborts the scan in
  //   the driver.
  //   _modeSyncReq — something ended (a scan's results freed, a join verdict,
  //   a forget) that may have left the radio holding more than the settings
  //   ask for; tick() re-asserts settingsWifiMode() at its next pass with
  //   nothing in flight and nothing asked for. Raisers put their state down
  //   before the flag and tick() drops the flag before reading any, so a
  //   request cannot slip through the check-then-act window the old
  //   cross-task restore had.
  volatile bool   _scanReq     = false;
  volatile bool   _joinReq     = false;
  volatile bool   _scanActive  = false;
  volatile bool   _modeSyncReq = false;
};

extern WifiManager wifiManager;
