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
//  WifiManager.cpp — see WifiManager.h for the service overview.
// ============================================================================
#include "WifiManager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include "LoRaRadio.h"
#include "Neighbors.h"

WifiManager wifiManager;

static const char PORTAL_URL[] = "http://10.42.0.1/";
static const char ADMIN_USER[] = "admin";

// ---------------------------------------------------------------------------
// Body accumulation for JSON POSTs. Chunks land in request->_tempObject,
// which the request destructor free()s. Returns the complete body on the
// final chunk, nullptr while more is coming (or after sending an error).
// ---------------------------------------------------------------------------
static const char* collectBody(AsyncWebServerRequest* request, const uint8_t* data,
                               size_t len, size_t index, size_t total) {
  if (total == 0 || total > 2048) {
    if (index == 0) request->send(413, "application/json", "{\"error\":\"too large\"}");
    return nullptr;
  }
  if (index == 0) request->_tempObject = malloc(total + 1);
  auto* body = static_cast<char*>(request->_tempObject);
  if (body == nullptr) {
    if (index + len >= total) request->send(500);
    return nullptr;
  }
  memcpy(body + index, data, len);
  if (index + len < total) return nullptr;
  body[total] = '\0';
  return body;
}

static void sendJson(AsyncWebServerRequest* r, int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  r->send(code, "application/json", out);
}

static void sendError(AsyncWebServerRequest* r, int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(r, code, d);
}

// ---------------------------------------------------------------------------
void WifiManager::begin() {
  startAccessPoint();

  // Captive portal: answer every DNS query with our own address. The OS
  // connectivity probes then hit port 80 and get redirected below, which
  // pops the "sign in to network" sheet on Android/iOS/Windows.
  _dns.setErrorReplyCode(DNSReplyCode::NoError);
  _dns.setTTL(60);
  _dns.start(53, "*", AP_IP);

  setupRoutes();
  _http.begin();

  log_i("SoftAP \"%s\" (%s) up at %s (http:%d, rns:%d)", _ssid, _securityName,
        WiFi.softAPIP().toString().c_str(), HTTP_PORT, RNS_TCP_PORT);
}

void WifiManager::startAccessPoint() {
  const WifiSettings& w = settings.wifi();

  #ifdef AP_SSID
    strlcpy(_ssid, AP_SSID, sizeof(_ssid));
  #else
    if (w.ssid[0] != '\0') {
      strlcpy(_ssid, w.ssid, sizeof(_ssid));
    } else {
      // Factory base MAC from efuse: stable across boots and identical to
      // the STA MAC (the SoftAP MAC is base+1, so it is deliberately not
      // used).
      uint64_t mac = ESP.getEfuseMac();    // little-endian: octet 0 in the LSB
      snprintf(_ssid, sizeof(_ssid), "%s-%02X%02X%02X", AP_SSID_PREFIX,
               (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
    }
  #endif

  // WPA needs 8..63 characters; anything else means an open network.
  bool secured = w.security != ApSecurity::Open && strlen(w.password) >= 8;
  const char* pass = secured ? w.password : nullptr;

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(_ssid, pass, w.channel, w.hidden ? 1 : 0, w.maxStations);
  _securityName = secured ? "wpa2" : "open";

  // The Arduino wrapper only knows open/WPA2. WPA3 (SAE) is set through
  // ESP-IDF: in IDF 4.4 the AP config accepts WPA2_WPA3_PSK / WPA3_PSK as
  // auth modes (cipher forced to CCMP, PMF implied by SAE). Mixed mode
  // lets WPA2-only clients still join.
  if (secured && w.security != ApSecurity::WPA2 && !WPA3_SOFTAP_SUPPORTED) {
    log_w("WPA3 needs an ESP-IDF 5 core; this build runs the AP as WPA2");
  } else if (secured && w.security != ApSecurity::WPA2) {
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK) {
      bool wpa3only = w.security == ApSecurity::WPA3;
      conf.ap.authmode = wpa3only ? WIFI_AUTH_WPA3_PSK : WIFI_AUTH_WPA2_WPA3_PSK;
      conf.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
      esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &conf);
      if (err == ESP_OK) {
        _securityName = wpa3only ? "wpa3" : "wpa2wpa3";
      } else {
        log_w("WPA3 mode rejected by the Wi-Fi driver (err 0x%x) — staying on WPA2", err);
      }
    }
  }
}

void WifiManager::tick() {
  if (_restartAt && (int32_t)(millis() - _restartAt) >= 0) {
    log_w("restarting to apply settings");
    delay(50);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// DNSServer is poll-driven; this task is pinned to CORE 0 by main.cpp so
// all captive-portal work stays off the radio core.
// ---------------------------------------------------------------------------
void WifiManager::dnsTask(void* self) {
  auto* wm = static_cast<WifiManager*>(self);
  for (;;) {
    wm->_dns.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------------------------------------------------------------------
// HTTP Basic Auth against the admin password. Sends the 401 challenge
// itself when it fails, so callers just `return`.
// ---------------------------------------------------------------------------
bool WifiManager::authed(AsyncWebServerRequest* request) {
  if (request->authenticate(ADMIN_USER, settings.admin().password)) return true;
  request->requestAuthentication();
  return false;
}

// ---------------------------------------------------------------------------
void WifiManager::setupRoutes() {
  // Handlers are matched in registration order: API and the protected
  // page first, then the static handler for everything else in LittleFS.
  _http.on("/api/status", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleStatus(r); });

  _http.on("/api/board", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleBoardGet(r); });
  _http.on("/api/board", HTTP_POST,
           [](AsyncWebServerRequest* r) {
             if (r->contentLength() == 0) sendError(r, 400, "empty");
           }, nullptr,
           [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
             if (const char* body = collectBody(r, d, l, i, t)) handleBoardPost(r, body, t);
           });

  // ---- admin -------------------------------------------------------------
  _http.on("/settings.html", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;               // browser prompts; fetches reuse the creds
    r->send(LittleFS, "/settings.html", "text/html");
  });
  _http.on("/api/settings", HTTP_GET,
           [this](AsyncWebServerRequest* r) { if (authed(r)) handleSettingsGet(r); });

  struct Route { const char* path; void (WifiManager::*fn)(AsyncWebServerRequest*, const char*, size_t); };
  static const Route posts[] = {
    { "/api/settings/radio", &WifiManager::handleRadioPost },
    { "/api/settings/wifi",  &WifiManager::handleWifiPost  },
    { "/api/settings/admin", &WifiManager::handleAdminPost },
  };
  for (const Route& rt : posts) {
    auto fn = rt.fn;
    _http.on(rt.path, HTTP_POST,
             [this](AsyncWebServerRequest* r) {
               if (r->contentLength() == 0 && authed(r)) sendError(r, 400, "empty");
             }, nullptr,
             [this, fn](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
               const char* body = collectBody(r, d, l, i, t);
               if (body && authed(r)) (this->*fn)(r, body, t);
             });
  }
  _http.on("/api/settings/reset", HTTP_POST,
           [this](AsyncWebServerRequest* r) { if (authed(r)) handleReset(r); });

  // The single-page app lives in LittleFS (data/ -> `pio run -t uploadfs`).
  _http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // OS connectivity probes — a redirect (any non-204/200 answer) is what
  // makes the client OS open its captive-portal browser.
  for (const char* probe : { "/generate_204", "/gen_204",
                             "/hotspot-detect.html", "/connecttest.txt",
                             "/ncsi.txt", "/canonical.html", "/success.txt" }) {
    _http.on(probe, HTTP_GET,
             [](AsyncWebServerRequest* r) { r->redirect(PORTAL_URL); });
  }

  // Everything else (arbitrary hostnames typed by the user, probe paths
  // not listed above) also lands on the portal.
  _http.onNotFound([](AsyncWebServerRequest* r) { r->redirect(PORTAL_URL); });
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------
void WifiManager::handleStatus(AsyncWebServerRequest* request) {
  const RadioSettings& rs = settings.radio();
  JsonDocument doc;

  doc["firmware"]     = FW_NAME;
  doc["version"]      = FW_VERSION;
  doc["ssid"]         = _ssid;
  doc["security"]     = _securityName;
  doc["display"]      = g_stats.displayPresent;
  doc["uptime_s"]     = millis() / 1000;
  doc["heap_free"]    = ESP.getFreeHeap();
  doc["psram_free"]   = ESP.getFreePsram();

  JsonObject radio    = doc["radio"].to<JsonObject>();
  radio["online"]     = g_stats.radioOnline;
  radio["model"]      = g_stats.radioModel;
  radio["freq_mhz"]   = rs.freqMhz;
  radio["bw_khz"]     = rs.bwKhz;
  radio["sf"]         = rs.sf;
  radio["cr"]         = rs.cr;
  radio["tx_dbm"]     = rs.txDbm;
  radio["sync_word"]  = rs.syncWord;
  radio["preamble"]   = rs.preamble;
  radio["apply_error"]= g_stats.radioApplyError;
  radio["rssi"]       = g_stats.lastRssi;
  radio["snr"]        = g_stats.lastSnr;
  radio["rx_packets"] = g_stats.loraRxPackets;
  radio["tx_packets"] = g_stats.loraTxPackets;
  radio["rx_dropped"] = g_stats.loraRxDropped;

  radio["beacon_interval"] = rs.beaconInterval;
  radio["callsign"]   = loraRadio.callsign();
  radio["beacons_tx"] = g_stats.beaconsTx;
  radio["beacons_rx"] = g_stats.beaconsRx;

  JsonObject peers    = doc["peers"].to<JsonObject>();
  peers["rns_tcp"]    = g_stats.tcpClients;      // Reticulum clients on :4242
  peers["wifi_sta"]   = WiFi.softAPgetStationNum();
  peers["tcp_rx_packets"] = g_stats.tcpRxPackets;

  // Stations heard on the channel (beacons / RNode station IDs)
  Neighbor snap[MAX_NEIGHBORS];
  size_t n = neighbors.snapshot(snap, MAX_NEIGHBORS);
  JsonArray nb = doc["neighbors"].to<JsonArray>();
  uint32_t now = millis();
  for (size_t i = 0; i < n; i++) {
    JsonObject o = nb.add<JsonObject>();
    o["name"]    = snap[i].name;
    o["version"] = snap[i].version;
    o["kind"]    = snap[i].kind == NeighborKind::RetiMesh ? "retimesh" : "station-id";
    o["rssi"]    = snap[i].rssi;
    o["snr"]     = snap[i].snr;
    o["age_s"]   = (now - snap[i].lastSeen) / 1000;
    o["beacons"] = snap[i].beacons;
  }

  sendJson(request, 200, doc);
}

// ---------------------------------------------------------------------------
// Bulletin board — deliberately public and unencrypted; lives on this node
// only. Private traffic belongs on Reticulum, which this node cannot read.
// ---------------------------------------------------------------------------
void WifiManager::handleBoardGet(AsyncWebServerRequest* request) {
  String out = "[]";
  File f = LittleFS.open(BOARD_FILE, "r");
  if (f) { out = f.readString(); f.close(); }
  request->send(200, "application/json", out);
}

void WifiManager::handleBoardPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || !in["text"].is<const char*>()) {
    sendError(request, 400, "bad json");
    return;
  }

  String author = in["author"] | "anonymous";
  String text   = in["text"].as<String>();
  author.trim(); text.trim();
  if (text.isEmpty()) { sendError(request, 400, "empty"); return; }
  if (author.isEmpty())                   author = "anonymous";
  if (author.length() > BOARD_MAX_AUTHOR) author = author.substring(0, BOARD_MAX_AUTHOR);
  if (text.length()   > BOARD_MAX_TEXT)   text   = text.substring(0, BOARD_MAX_TEXT);

  // All HTTP handlers run on the single AsyncTCP task: no file locking needed.
  JsonDocument boardDoc;
  {
    File f = LittleFS.open(BOARD_FILE, "r");
    if (f) { deserializeJson(boardDoc, f); f.close(); }
  }
  JsonArray posts = boardDoc.as<JsonArray>();
  if (posts.isNull()) posts = boardDoc.to<JsonArray>();

  uint32_t nextId = 1;
  for (JsonObject p : posts) nextId = max(nextId, p["id"].as<uint32_t>() + 1);

  JsonObject post = posts.add<JsonObject>();
  post["id"]     = nextId;               // ordering only — the node has no RTC
  post["author"] = author;
  post["text"]   = text;
  while (posts.size() > BOARD_MAX_POSTS) posts.remove(0);

  File f = LittleFS.open(BOARD_FILE, "w");
  if (!f) { sendError(request, 500, "fs"); return; }
  serializeJson(posts, f);
  f.close();
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// Settings API (all authenticated)
// ---------------------------------------------------------------------------
void WifiManager::handleSettingsGet(AsyncWebServerRequest* request) {
  const RadioSettings& rs = settings.radio();
  const WifiSettings&  ws = settings.wifi();
  JsonDocument doc;

  JsonObject radio   = doc["radio"].to<JsonObject>();
  radio["freq_mhz"]  = rs.freqMhz;
  radio["bw_khz"]    = rs.bwKhz;
  radio["sf"]        = rs.sf;
  radio["cr"]        = rs.cr;
  radio["tx_dbm"]    = rs.txDbm;
  radio["tx_dbm_max"]= loraRadio.online() ? loraRadio.maxTxDbm() : 22;
  radio["sync_word"] = rs.syncWord;
  radio["preamble"]  = rs.preamble;
  radio["beacon_interval"] = rs.beaconInterval;
  radio["callsign"]  = rs.callsign;              // "" = SSID
  radio["callsign_active"] = loraRadio.callsign();
  radio["model"]     = g_stats.radioModel;
  radio["online"]    = g_stats.radioOnline;
  radio["apply_error"] = g_stats.radioApplyError;

  JsonObject wifi    = doc["wifi"].to<JsonObject>();
  wifi["ssid"]       = ws.ssid;            // "" = automatic
  wifi["ssid_active"]= _ssid;
  wifi["security"]   = Settings::securityName(ws.security);
  wifi["security_active"] = _securityName;
  wifi["wpa3_supported"] = (bool)WPA3_SOFTAP_SUPPORTED;
  wifi["has_password"] = strlen(ws.password) >= 8;
  wifi["channel"]    = ws.channel;
  wifi["max_stations"] = ws.maxStations;
  wifi["hidden"]     = ws.hidden;

  doc["admin"]["user"] = ADMIN_USER;
  doc["admin"]["default_password"] = strcmp(settings.admin().password, ADMIN_PASSWORD_DEFAULT) == 0;

  sendJson(request, 200, doc);
}

void WifiManager::handleRadioPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }

  RadioSettings r = settings.radio();
  if (in["freq_mhz"].is<float>())  r.freqMhz  = in["freq_mhz"];
  if (in["bw_khz"].is<float>())    r.bwKhz    = in["bw_khz"];
  if (in["sf"].is<int>())          r.sf       = in["sf"];
  if (in["cr"].is<int>())          r.cr       = in["cr"];
  if (in["tx_dbm"].is<int>())      r.txDbm    = in["tx_dbm"];
  if (in["sync_word"].is<int>())   r.syncWord = in["sync_word"];
  if (in["preamble"].is<int>())    r.preamble = in["preamble"];
  if (in["beacon_interval"].is<int>()) r.beaconInterval = in["beacon_interval"];
  if (in["callsign"].is<const char*>()) {
    String c = in["callsign"].as<String>(); c.trim();
    for (size_t i = 0; i < c.length(); i++)
      if (c[i] < 0x21 || c[i] > 0x7E) { sendError(request, 400, "callsign: printable ASCII without spaces only"); return; }
    if (c.length() > 32) { sendError(request, 400, "callsign must be at most 32 characters"); return; }
    strlcpy(r.callsign, c.c_str(), sizeof(r.callsign));
  }

  int8_t maxDbm = loraRadio.online() ? loraRadio.maxTxDbm() : 22;
  if (r.freqMhz < 137.0f || r.freqMhz > 1020.0f) { sendError(request, 400, "frequency must be 137-1020 MHz"); return; }
  if (!Settings::validBandwidth(r.bwKhz))          { sendError(request, 400, "unsupported bandwidth"); return; }
  if (r.sf < 7 || r.sf > 12)                       { sendError(request, 400, "spreading factor must be 7-12"); return; }
  if (r.cr < 5 || r.cr > 8)                        { sendError(request, 400, "coding rate must be 5-8 (4/5..4/8)"); return; }
  if (r.txDbm < 2 || r.txDbm > maxDbm)             { sendError(request, 400, "tx power out of range for this transceiver"); return; }
  if (r.preamble < 6 || r.preamble > 1000)         { sendError(request, 400, "preamble must be 6-1000 symbols"); return; }
  if (r.beaconInterval != 0 && (r.beaconInterval < 10 || r.beaconInterval > 3600)) { sendError(request, 400, "beacon interval must be 0 (off) or 10-3600 s"); return; }

  if (!settings.saveRadio(r)) { sendError(request, 500, "nvs"); return; }
  if (loraRadio.online()) loraRadio.requestReconfigure(r);

  JsonDocument out;
  out["ok"] = true;
  out["applied"] = loraRadio.online();
  sendJson(request, 200, out);
}

void WifiManager::handleWifiPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }

  WifiSettings w = settings.wifi();
  if (in["ssid"].is<const char*>()) {
    String s = in["ssid"].as<String>(); s.trim();
    if (s.length() > 32) { sendError(request, 400, "ssid must be at most 32 characters"); return; }
    strlcpy(w.ssid, s.c_str(), sizeof(w.ssid));
  }
  if (in["security"].is<const char*>() && !Settings::securityFromName(in["security"], w.security)) {
    sendError(request, 400, "security must be open|wpa2|wpa2wpa3|wpa3"); return;
  }
  if (in["password"].is<const char*>()) {
    const char* p = in["password"];
    if (p[0] != '\0') {                    // empty = keep the stored password
      size_t pl = strlen(p);
      if (pl < 8 || pl > 63) { sendError(request, 400, "password must be 8-63 characters"); return; }
      strlcpy(w.password, p, sizeof(w.password));
    }
  }
  if (in["channel"].is<int>())      w.channel     = in["channel"];
  if (in["max_stations"].is<int>()) w.maxStations = in["max_stations"];
  if (in["hidden"].is<bool>())      w.hidden      = in["hidden"];

  if (w.security != ApSecurity::Open && strlen(w.password) < 8) { sendError(request, 400, "a password is required for a secured network"); return; }
  if (w.channel < 1 || w.channel > 13)          { sendError(request, 400, "channel must be 1-13"); return; }
  if (w.maxStations < 1 || w.maxStations > 10)  { sendError(request, 400, "max stations must be 1-10"); return; }

  if (!settings.saveWifi(w)) { sendError(request, 500, "nvs"); return; }

  JsonDocument out;
  out["ok"] = true;
  out["restart"] = true;
  out["ssid"] = w.ssid[0] ? w.ssid : _ssid;   // auto-derived name does not change
  out["security"] = Settings::securityName(w.security);
  sendJson(request, 200, out);
  scheduleRestart(1500);                 // let the response leave first
}

void WifiManager::handleAdminPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || !in["password"].is<const char*>()) {
    sendError(request, 400, "bad json"); return;
  }
  const char* p = in["password"];
  size_t pl = strlen(p);
  if (pl < 4 || pl > 32) { sendError(request, 400, "password must be 4-32 characters"); return; }
  if (!settings.saveAdminPassword(p)) { sendError(request, 500, "nvs"); return; }
  request->send(200, "application/json", "{\"ok\":true}");
}

void WifiManager::handleReset(AsyncWebServerRequest* request) {
  settings.factoryReset();
  request->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
  scheduleRestart(1500);
}
