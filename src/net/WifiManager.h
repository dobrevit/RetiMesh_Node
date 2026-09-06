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
//  Wi-Fi *settings* changes are saved and followed by a scheduled restart,
//  because reconfiguring the AP drops the very connection the request came
//  on. The AP's *switch* (links.wifi_ap) and the idle policy are the
//  exception: they only take the AP up or down, never reshape it, and ride
//  tick()'s convergence with a short grace so the reply leaves first.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "ApIdlePolicy.h"
#include "SampleGate.h"
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

  // --- AP idle auto-off (ApIdlePolicy.h) ----------------------------------
  // The one wake entry. Every wake source asks here — the button on the
  // display task, the console's WIFI ON even when the settings are unchanged
  // (and with it an admin message, which runs the same parser), the AP
  // switch being asked on. Raise-only, safe from any task; tick() serves it
  // on the loop task, where the policy and the driver live. A station
  // cannot wake a suppressed AP: the AP is down and beacon-less, so there
  // is nothing to associate to.
  void apWake() { _apWakeReq = true; }
  // A link switch the running radio can follow changed — links.wifi_ap,
  // which LocalLink::applyLinks now applies live. Raise-only, any task;
  // tick() converges, staging a short grace before any AP teardown so the
  // reply that asked leaves first.
  void requestModeSync() { _modeSyncReq = true; }
  // Whether the idle policy is holding the access point down. "Down by
  // policy" and "off by switch" must stay distinguishable on every status
  // surface: a wake brings the first back, only the operator the second.
  bool apIdleDown() const { return _apSuppressed; }
  // The AP against its switch, in one word: "up", "down" (enabled, not on
  // the air — starting, or inside the teardown grace), "idle-off", "off".
  const char* apStateName() const;

  // Re-applies wifi.sta_listen_interval to the station config, by
  // read-modify-write. staConnect() calls it in the quiet gap it opens
  // between writing the config and connecting, the STA_CONNECTED hook
  // re-applies it after every association, and the settings commit calls it
  // for a live change. Public for the commit's sake
  // (SettingsFields::commitWifi); safe to call with no station up (it does
  // nothing then).
  void applyStaListenInterval();
  // A Wi-Fi save just landed whose restart-applied fields — security among
  // them — the running AP must NOT follow until the reboot. tick() drops any
  // pending AP config patch retry: applyApConfigPatch re-reads
  // settings.wifi() on every attempt, so a retry armed before the save would
  // patch the next boot's security onto the AP the operator was told keeps
  // its shape. Raise-only, any task (SettingsFields::commitWifi calls it
  // from the console and AsyncTCP tasks); tick() serves it on the loop task
  // like the flags below.
  void cancelApConfigPatch() { _apPatchCancelReq = true; }

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
  // tick()'s convergence body: computes the shape the settings and the idle
  // policy ask for, stages the grace before an AP teardown, applies the
  // change on this task, and takes the dependent services (captive DNS,
  // mDNS's first start, AutoInterface) up or down on the same transition.
  void syncRadioShape();
  void apServicesDown();          // the AP is leaving the air
  void apServicesUp();            // the AP is back on it
  // The mDNS responder, started once — from begin() on a Wi-Fi boot, or
  // from the first runtime Wi-Fi up on a node that booted with it off.
  void startMdns();
  // The one station connect. Every site that starts a join — the boot's,
  // the glass's, the fallback after a failed join — goes through here, so
  // the listen interval is written into the config exactly once, between
  // the core building it and the connect being issued (see the definition).
  void staConnect(const char* ssid, const char* password);
  // The AP config read-modify-write (beacon interval, and WPA3 where asked):
  // startAccessPoint() applies it after softAP(), and tick() re-runs just
  // this when the driver refused it with ESP_ERR_WIFI_STATE — a station
  // connect in flight closes esp_wifi_set_config's window. Returns true when
  // the patch landed; inFlight reports that retryable refusal, and every
  // other failure is logged inside.
  bool applyApConfigPatch(bool& inFlight);
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
  //   _apWakeReq — somebody asked for the access point (apWake()). Volatile
  //   like the rest: raised from the display task or the AsyncTCP task,
  //   served by tick(). _apSuppressed below is tick()'s alone to write, but
  //   not loop-task-only to read: the status surfaces (handleStatus and
  //   handleSettingsGet on the AsyncTCP task, apStateName()/apIdleDown()
  //   from the console and the glass) read it, so it is volatile like the
  //   flags above. Everything after it is tick()'s own state, written and
  //   read on the loop task only.
  volatile bool   _apWakeReq   = false;
  ApIdlePolicy    _apIdle;               // fed by tick() at the gate's cadence
  // The 1 s station-count poll. Invariant: AP_STOP_GRACE_MS must exceed this
  // cadence, so at least one policy ask lands inside every teardown grace —
  // that ask is what re-reads the station count, lifts the suppression for a
  // station that associated during the grace, and drops the staged teardown
  // before it lands (syncRadioShape arms the stage AT a gate ask, so a
  // shorter grace expires before the next ask in a healthy loop).
  static constexpr uint32_t kApIdleGateMs = 1000;
  static_assert(AP_STOP_GRACE_MS > kApIdleGateMs,
                "the AP teardown grace must outlast the idle-gate cadence, or a station "
                "associating during the grace cannot cancel the teardown");
  SampleGate      _apIdleGate{kApIdleGateMs};
  volatile bool   _apSuppressed = false; // the policy's verdict, mirrored for settingsWifiMode
  bool            _apDownStaged = false; // a teardown is waiting out its grace
  uint32_t        _apDownDueMs  = 0;
  // The AP config patch that could not land yet: esp_wifi_set_config(AP)
  // refuses with ESP_ERR_WIFI_STATE while a station connect is in flight —
  // on a LAN-down node the 30 s watchdog keeps one going much of the time —
  // and a runtime AP re-up landing in that window would otherwise beacon at
  // 100 TU/WPA2 until the next cycle. Attempts left; 0 = none armed.
  uint8_t         _apPatchRetries = 0;
  SampleGate      _apPatchGate{2000};    // one retry every 2 s while armed
  // cancelApConfigPatch()'s ask — volatile like the request flags above:
  // raised from whatever task the settings commit runs on, served at the top
  // of tick() so a bring-up later in the same pass can re-arm afresh (its
  // patch is built from the settings the save just wrote, so it is current).
  volatile bool   _apPatchCancelReq = false;
  bool            _mdnsUp       = false; // startMdns() has run (this boot)
  bool            _autoIfEnded  = false; // we ended AutoInterface; re-begin on the way up
};

extern WifiManager wifiManager;
