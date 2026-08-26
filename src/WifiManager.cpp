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

WifiManager wifiManager;

static const char PORTAL_URL[] = "http://10.42.0.1/";

// ---------------------------------------------------------------------------
void WifiManager::begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);

  // Fewer than 8 password characters means WPA2 would reject it — run open.
  const char* pass = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;
  WiFi.softAP(AP_SSID, pass, AP_CHANNEL, 0, AP_MAX_STATIONS);

  // Captive portal: answer every DNS query with our own address. The OS
  // connectivity probes then hit port 80 and get redirected below, which
  // pops the "sign in to network" sheet on Android/iOS/Windows.
  _dns.setErrorReplyCode(DNSReplyCode::NoError);
  _dns.setTTL(60);
  _dns.start(53, "*", AP_IP);

  setupRoutes();
  _http.begin();

  log_i("SoftAP \"%s\" up at %s (http:%d, rns:%d)",
        AP_SSID, WiFi.softAPIP().toString().c_str(), HTTP_PORT, RNS_TCP_PORT);
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
void WifiManager::setupRoutes() {
  // The single-page app lives in LittleFS (data/ -> `pio run -t uploadfs`).
  _http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  _http.on("/api/status", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleStatus(r); });

  _http.on("/api/board", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleBoardGet(r); });

  _http.on("/api/board", HTTP_POST,
           [](AsyncWebServerRequest* r) {
             // Body handler below sends the real response; this only fires
             // first when there is no body at all.
             if (r->contentLength() == 0)
               r->send(400, "application/json", "{\"error\":\"empty\"}");
           },
           nullptr,
           [this](AsyncWebServerRequest* r, uint8_t* data, size_t len,
                  size_t index, size_t total) {
             handleBoardPost(r, data, len, index, total);
           });

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
  JsonDocument doc;

  doc["firmware"]     = FW_NAME;
  doc["version"]      = FW_VERSION;
  doc["uptime_s"]     = millis() / 1000;
  doc["heap_free"]    = ESP.getFreeHeap();
  doc["psram_free"]   = ESP.getFreePsram();

  JsonObject radio    = doc["radio"].to<JsonObject>();
  radio["online"]     = g_stats.radioOnline;
  radio["freq_mhz"]   = RF_FREQ_MHZ;
  radio["bw_khz"]     = RF_BW_KHZ;
  radio["sf"]         = RF_SF;
  radio["rssi"]       = g_stats.lastRssi;
  radio["snr"]        = g_stats.lastSnr;
  radio["rx_packets"] = g_stats.loraRxPackets;
  radio["tx_packets"] = g_stats.loraTxPackets;
  radio["rx_dropped"] = g_stats.loraRxDropped;

  JsonObject peers    = doc["peers"].to<JsonObject>();
  peers["rns_tcp"]    = g_stats.tcpClients;      // Reticulum clients on :4242
  peers["wifi_sta"]   = WiFi.softAPgetStationNum();
  peers["tcp_rx_packets"] = g_stats.tcpRxPackets;

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// GET /api/board — the file already is the JSON response.
// Read fully into RAM (it is capped small) so a concurrent POST can never
// rewrite the file under a streaming response.
// ---------------------------------------------------------------------------
void WifiManager::handleBoardGet(AsyncWebServerRequest* request) {
  String out = "[]";
  File f = LittleFS.open(BOARD_FILE, "r");
  if (f) { out = f.readString(); f.close(); }
  request->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// POST /api/board  {"author": "...", "text": "..."}
//
// The board is intentionally UNENCRYPTED community space — it lives on
// this node only and is readable by anyone who joins the AP. Private
// communication belongs on the Reticulum side, where it is end-to-end
// encrypted and this node cannot read it at all.
// ---------------------------------------------------------------------------
void WifiManager::handleBoardPost(AsyncWebServerRequest* request,
                                  const uint8_t* data, size_t len,
                                  size_t index, size_t total) {
  if (total > 1024) {
    if (index == 0)
      request->send(413, "application/json", "{\"error\":\"too large\"}");
    return;
  }

  // Body can arrive in chunks; accumulate in _tempObject, which the
  // request destructor free()s for us.
  if (index == 0) request->_tempObject = malloc(total + 1);
  auto* body = static_cast<char*>(request->_tempObject);
  if (body == nullptr) { request->send(500); return; }
  memcpy(body + index, data, len);
  if (index + len < total) return;       // wait for the rest
  body[total] = '\0';

  JsonDocument in;
  if (deserializeJson(in, body, total) != DeserializationError::Ok ||
      !in["text"].is<const char*>()) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }

  String author = in["author"] | "anonymous";
  String text   = in["text"].as<String>();
  author.trim(); text.trim();
  if (text.isEmpty()) {
    request->send(400, "application/json", "{\"error\":\"empty\"}");
    return;
  }
  if (author.isEmpty())                   author = "anonymous";
  if (author.length() > BOARD_MAX_AUTHOR) author = author.substring(0, BOARD_MAX_AUTHOR);
  if (text.length()   > BOARD_MAX_TEXT)   text   = text.substring(0, BOARD_MAX_TEXT);

  // Load, append, rotate, persist. All HTTP handlers run on the single
  // AsyncTCP task, so file access here needs no extra locking.
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
  if (!f) { request->send(500, "application/json", "{\"error\":\"fs\"}"); return; }
  serializeJson(posts, f);
  f.close();

  request->send(200, "application/json", "{\"ok\":true}");
}
