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
#include <sys/stat.h>
#include "QrCode.h"
#include "Pmu.h"
#include "Gps.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include "LoRaRadio.h"
#include "Neighbors.h"
#include "RnsAnnounce.h"
#include "RnsTransport.h"
#include "Mdns.h"
#include "Diag.h"
#include "SdCard.h"
#include "StoreHome.h"
#include "AutoInterface.h"
#include "Power.h"
#include "LocalLink.h"
#include "Bootloader.h"

WifiManager wifiManager;

// One answer for every write path that has to refuse while a restart is on
// its way: a write accepted now may or may not reach NVS before it, and the
// caller could not tell which.
static const char kRestartingMsg[] = "the node is restarting";

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
bool WifiManager::wifiEnabled() const { return settings.links().wifiEnabled; }

// The bootloader plan and state, written once. Three handlers used to spell
// this out separately and had already drifted: the same fact was api_enabled
// in two of them and bootloader_api in the others, methods in one and
// bootloader_methods in another, so a tool reading two endpoints needed two
// parsers for one thing.
static void bootloaderJson(JsonObject o) {
  const Bootloader::Plan p = Bootloader::plan();
  const Bootloader::Pending r = Bootloader::snapshot();
  o["software_entry"] = p.has(Bootloader::Method::SoftwareApi);
  o["api_enabled"]    = settings.maintenance().bootloaderApi;
  o["pending"]        = r.armed();
  o["state"]          = Bootloader::stateName(r.state);
  if (r.armed()) {
    o["target"]    = Bootloader::targetName(r.target);
    o["source"]    = Bootloader::sourceName(r.source);
    o["due_in_ms"] = r.dueInMs(millis());
  }
  o["primary"]        = Bootloader::methodName(p.primary());
  const Diag::LastRestart lr = Diag::boot().lastRestart;
  if (lr.known) {
    JsonObject last = o["last_restart"].to<JsonObject>();
    last["to_persist_ms"] = lr.toPersistMs;
    last["to_boot_ms"]    = lr.toBootMs;
  }
  o["recovery"]       = Bootloader::manualRecovery();
  JsonArray methods = o["methods"].to<JsonArray>();
  for (size_t i = 0; i < p.count; i++) methods.add(Bootloader::methodName(p.methods[i]));
}

// Whether this request came in over a host-facing link: judged by the
// address it was accepted at and the address it came from, together. See
// LocalLink::requestIsHostFacing for why neither is enough on its own.
static bool fromHostFacingLink(AsyncWebServerRequest* r) {
  return LocalLink::requestIsHostFacing(LocalLink::hostOrder(r->client()->localIP()),
                                        LocalLink::hostOrder(r->client()->remoteIP()));
}

// The maintenance switches, bound to their keys once. The same table shape
// LocalLink::fields() gives the links, for the same reason: four handlers
// spelled these three keys out by hand, and the fourth copy is the one that
// gets missed when a switch is added.
struct MaintField { const char* key; bool MaintenanceSettings::*on; };
static const MaintField kMaintFields[] = {
  { "bootloader_api",      &MaintenanceSettings::bootloaderApi },
  { "bootloader_from_lan", &MaintenanceSettings::bootloaderFromLan },
  { "console_enabled",     &MaintenanceSettings::consoleEnabled },
};

void WifiManager::begin() {
  if (wifiEnabled()) {
    startAccessPoint();

    // Captive portal: answer every DNS query on the access point with our
    // own address. The OS connectivity probes then hit port 80 and get
    // redirected below, which pops the "sign in to network" sheet on
    // Android/iOS/Windows. Bound to the AP's address, not to every
    // interface: a host on the USB link must not have its names steered
    // here (CaptiveDns.h).
  } else {
    // Wi-Fi off is a configuration, not a failure: the names are still
    // derived (the display and the console show them) and the radio is left
    // down. The web server below starts regardless, bound to every interface,
    // so a USB or PPP link — or WIFI ON at the console — reaches it.
    resolveNames();
    // lwIP and the netif layer have to exist for the servers below to bind.
    // Network.begin() is the core's own stack bring-up — esp_netif, the
    // default event loop and the event task — and the first thing
    // WiFi.mode() does before it touches the driver. On core 2 that step had
    // no public name; calling the private one on its own overflowed the IPC
    // task on core 1 once the radio attached its receive interrupt, and the
    // driver was started and stopped again just to get past it. Core 3
    // publishes the step, so the node asks for it and leaves the radio down.
    // What the driver's start-up did for the IPC task was never named, so
    // this path stands on a real boot with Wi-Fi off and not on an argument.
    Network.begin();
    log_w("Wi-Fi is switched off in settings; the access point will not start");
  }

  setupRoutes();
  // Where the USB link exists the resolver runs whether or not Wi-Fi does:
  // the link's lease names the node as DNS (CaptiveDns.h), and with the
  // access point off there would otherwise be nothing on port 53 to refuse
  // the host's queries — a timeout where a refusal was promised. A board
  // with neither has nothing to answer, and AsyncUDP's task goes unmade.
  if (wifiEnabled() || HAS_USB_NCM) {
    if (!_dns.begin(AP_IP)) log_w("captive DNS: could not bind port 53");
  }
  _http.begin();

  // http://retimesh.local/ (and http://<ssid>.local/) for clients whose
  // captive-portal detection does not fire.
  // Compare the stamp baked into this firmware against the one in the image it
  // is serving. They are produced together by tools/asset_stamp.py, so a
  // mismatch means only one half was flashed — the portal will be subtly wrong
  // in ways nothing else reports.
  {
    File f = LittleFS.open("/assets.json", "r");
    if (f) {
      JsonDocument sd;
      if (deserializeJson(sd, f) == DeserializationError::Ok) _assetStamp = sd["stamp"] | "";
      f.close();
    }
    if (_assetStamp == ASSET_STAMP) {
      log_i("web assets match this firmware (build %s)", ASSET_STAMP);
    } else {
      log_w("web assets were built from a different firmware: image has \"%s\", firmware "
            "expects \"%s\". The portal may be missing controls this firmware supports, or "
            "offer some it does not. Upload the filesystem to match — note that doing so "
            "erases anything else on it, including the Reticulum store on boards with no "
            "SD card.",
            _assetStamp.isEmpty() ? "(none)" : _assetStamp.c_str(), ASSET_STAMP);
    }
  }

  deriveHostname();
  if (!wifiEnabled()) {
    log_i("HTTP :%d and RNS TCP :%d listening on every local link; Wi-Fi off", HTTP_PORT, RNS_TCP_PORT);
    return;
  }
  if (MDNS.begin(_hostname)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    MDNS.addService("rns", "tcp", RNS_TCP_PORT);
    // So a browser or a script can tell the nodes apart without opening each
    // one: the same identity the portal and the announce carry.
    for (const char* svc : { "http", "rns" }) {
      MDNS.addServiceTxt(svc, "tcp", "name",  _ssid);
      MDNS.addServiceTxt(svc, "tcp", "node",  nodeIdentity.destHex());
      MDNS.addServiceTxt(svc, "tcp", "fw",    FW_VERSION);
      MDNS.addServiceTxt(svc, "tcp", "board", BOARD_NAME);
    }
    log_i("mDNS: http://%s.local (rns on :%d) — browse _rns._tcp to find every node",
          _hostname, RNS_TCP_PORT);
  } else {
    log_w("mDNS start failed");
  }

  log_i("SoftAP \"%s\" (%s) up at %s (http:%d, rns:%d)", _ssid, _securityName,
        WiFi.softAPIP().toString().c_str(), HTTP_PORT, RNS_TCP_PORT);
}

// What this node calls itself, worked out without starting anything. The store
// migration runs before the radios are up and writes the node's name onto the
// card it is claiming, and it used to run after this — so the name went on the
// card empty, and a card whose owner had no name is a card a person cannot
// identify in their hand. Idempotent, and startAccessPoint() still calls it, so
// there is one derivation rather than an early copy and a real one.
void WifiManager::resolveNames() {
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
  deriveHostname();
}

void WifiManager::startAccessPoint() {
  const WifiSettings& w = settings.wifi();
  resolveNames();

  // Every node used to answer to the same "retimesh.local", so the second one
  // on a network either lost the race or was silently renamed by conflict
  // resolution to something nobody could predict — which made more than one
  // node on one LAN unusable. The access-point name is already unique per node
  // and already what the display and the portal show, so the mDNS name is that
  // name rather than a second derivation from the MAC that could drift from it.
  //
  // mDNS labels are letters, digits and hyphens, compared without regard to
  // case, so an SSID someone has renamed to "Shed roof" still yields a legal
  // "shed-roof.local".

  // WPA needs 8..63 characters; anything else means an open network.
  bool secured = w.security != ApSecurity::Open && strlen(w.password) >= 8;
  const char* pass = secured ? w.password : nullptr;

  WiFi.persistent(false);
  WiFi.mode(stationConfigured() ? WIFI_AP_STA : WIFI_AP);
  // IPv6 link-local on both links, asked for before they start: core 3
  // creates the address when the interface comes up and only then, so a
  // request made afterwards — which is when AutoInterface used to make it —
  // waits for a start that has already happened. AutoInterface reads the
  // addresses; the links are brought up here, so they are enabled here.
  WiFi.softAPenableIPv6();
  WiFi.enableIPv6();
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(_ssid, pass, w.channel, w.hidden ? 1 : 0, w.maxStations);

  // Station mode: join the configured LAN too. The AP and the STA share
  // one radio, so the AP follows the LAN's channel once connected.
  if (stationConfigured()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(w.staSsid, w.staPassword[0] ? w.staPassword : nullptr);
    log_i("station: joining \"%s\"", w.staSsid);
    _staRetryAt = millis() + 30000;
  }
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
  // Station watchdog: log transitions, kick a reconnect if auto-reconnect
  // gave up (e.g. the LAN was down at boot).
  if (wifiEnabled() && stationConfigured()) {
    static bool wasConnected = false;
    bool now = stationConnected();
    if (now != wasConnected) {
      wasConnected = now;
      if (now) log_i("station: connected to \"%s\", IP %s, RSSI %d dBm", settings.wifi().staSsid, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      else     log_w("station: disconnected from \"%s\"", settings.wifi().staSsid);
    }
    if (!now && (int32_t)(millis() - _staRetryAt) >= 0) {
      _staRetryAt = millis() + 30000;
      WiFi.reconnect();
    }
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

  // QR codes as SVG. "wifi" embeds the AP password, so it needs the admin
  // credentials like every other place that reveals it; the portal URL and
  // the node address are public.
  _http.on("/api/qr", HTTP_GET, [this](AsyncWebServerRequest* r) {
    Qr::Payload what;
    if (!Qr::parsePayload(r->hasParam("what") ? r->getParam("what")->value().c_str() : "wifi", what)) {
      sendError(r, 400, "what must be wifi, portal or address"); return;
    }
    if (what == Qr::Payload::Wifi && settings.wifi().security != ApSecurity::Open && !authed(r)) return;
    handleQrFor(r, what);
  });
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

  struct Route { const char* path; void (WifiManager::*fn)(AsyncWebServerRequest*, const char*, size_t); bool ungated; };   // trailing member: rows that omit it are gated (value-initialised false)
  static const Route posts[] = {
    { "/api/settings/radio", &WifiManager::handleRadioPost },
    { "/api/settings/wifi",  &WifiManager::handleWifiPost  },
    { "/api/settings/admin", &WifiManager::handleAdminPost },
    { "/api/settings/transport", &WifiManager::handleTransportPost },
    { "/api/settings/sd/format", &WifiManager::handleSdFormat },
    { "/api/settings/sd/adopt",  &WifiManager::handleSdAdopt  },
    { "/api/settings/sd/eject",  &WifiManager::handleSdEject  },
    { "/api/settings/import", &WifiManager::handleImport },
    { "/api/settings/links", &WifiManager::handleLinksPost },
    { "/api/settings/maintenance", &WifiManager::handleMaintenancePost },
    // System: privileged, POST only, and — unlike the settings above — also
    // gated on which link the request came over. See handleBootloaderPost.
    // The bootloader request is the one POST the restart gate does not
    // cover: the sequencer lets it outrank a plain reboot already armed (a
    // flashing tool that asks during a settings save means it), and a 503
    // here would have contradicted that.
    { "/api/system/bootloader", &WifiManager::handleBootloaderPost, true },
    { "/api/system/reboot",     &WifiManager::handleRebootPost },
  };
  for (const Route& rt : posts) {
    auto fn = rt.fn;
    const bool gated = !rt.ungated;
    _http.on(rt.path, HTTP_POST,
             [this](AsyncWebServerRequest* r) {
               if (r->contentLength() == 0 && authed(r)) sendError(r, 400, "empty");
             }, nullptr,
             [this, fn, gated](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
               const char* body = collectBody(r, d, l, i, t);
               if (!body || !authed(r)) return;
               if (gated && Bootloader::pending()) { sendError(r, 503, kRestartingMsg); return; }
               (this->*fn)(r, body, t);
             });
  }
  // What this board can do about its bootloader, for tooling. No secrets:
  // the same facts are in boards.json.
  _http.on("/api/system/bootloader", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleBootloaderGet(r); });
  // Event log from the SD card (admin). ?prev=1 serves the rotated file.
  _http.on("/api/sd/log", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    if (!sdCard.mounted()) { sendError(r, 404, "no card mounted"); return; }
    const char* path = r->hasParam("prev") ? SdCard::LOG_PREV_PATH : SdCard::LOG_PATH;
    if (!SD.exists(path)) { sendError(r, 404, "no log yet"); return; }
    AsyncWebServerResponse* res = r->beginResponse(SD, path, "text/plain");
    res->addHeader("Content-Disposition", "attachment; filename=\"retimesh-events.log\"");
    r->send(res);
  });

  _http.on("/api/settings/export", HTTP_GET,
           [this](AsyncWebServerRequest* r) { if (authed(r)) handleExport(r); });
  // Takes no body, so it is not in the table above — but it writes NVS,
  // so it stands behind the same gate.
  _http.on("/api/settings/reset", HTTP_POST,
           [this](AsyncWebServerRequest* r) {
             if (!authed(r)) return;
             if (Bootloader::pending()) { sendError(r, 503, kRestartingMsg); return; }
             handleReset(r);
           });

  // OS connectivity probes — a redirect (any non-204/200 answer) is what
  // makes the client OS open its captive-portal browser. Only where there
  // is an access point to be captive on: with Wi-Fi off a host on any other
  // link would be sent to an address that does not exist.
  if (wifiEnabled()) {
    for (const char* probe : { "/generate_204", "/gen_204",
                               "/hotspot-detect.html", "/connecttest.txt",
                               "/ncsi.txt", "/canonical.html", "/success.txt" }) {
      _http.on(probe, HTTP_GET,
               [](AsyncWebServerRequest* r) { r->redirect(PORTAL_URL); });
    }
  }

  // The single-page app lives in LittleFS (data/ -> `pio run -t uploadfs`).
  // Explicit routes for the two hottest paths avoid the static handler's
  // .gz / directory probes (each one logs a VFS error at debug level 3).
  _http.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(LittleFS, "/index.html", "text/html"); });
  _http.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(204); });
  _http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // Everything else (arbitrary hostnames typed by the user, probe paths
  // not listed above) also lands on the portal — when there is one.
  if (wifiEnabled()) _http.onNotFound([](AsyncWebServerRequest* r) { r->redirect(PORTAL_URL); });
  else               _http.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "not found"); });
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
  doc["hostname"]     = _hostname;      // reachable as <hostname>.local
  doc["security"]     = _securityName;
  {
    JsonObject st = doc["station"].to<JsonObject>();
    st["configured"] = stationConfigured();
    st["ssid"]       = settings.wifi().staSsid;
    st["connected"]  = stationConnected();
    st["ip"]         = stationConnected() ? WiFi.localIP().toString() : "";
    st["rssi"]       = stationConnected() ? WiFi.RSSI() : 0;
  }
  doc["display"]      = g_stats.displayPresent;
  // Firmware and web assets are flashed separately and nothing forces them to
  // be updated together, so a node can end up serving a portal built against a
  // different API and look entirely healthy doing it. Both halves carry the
  // same hash when they are built together; publishing both lets anyone see at
  // a glance whether this node is one build or two.
  {
    JsonObject as = doc["assets"].to<JsonObject>();
    as["firmware"] = ASSET_STAMP;
    as["filesystem"] = _assetStamp;
    as["match"] = (_assetStamp == ASSET_STAMP);
  }
  doc["identity"]     = nodeIdentity.identityHex();
  doc["destination"]  = nodeIdentity.destHex();      // retimesh.node
  doc["uptime_s"]     = millis() / 1000;
  {
    Power::Battery b = Power::battery();
    JsonObject pw = doc["power"].to<JsonObject>();
    pw["profile"] = Power::profileName(Power::profile());
    pw["cpu_mhz"] = getCpuFrequencyMhz();
    pw["battery_present"] = b.present;
    // Null, not false, where the board has no way to tell. A caller can then
    // say "unknown" instead of drawing a conclusion this node never reached.
    if (b.chargeKnown) pw["battery_charging"] = b.charging;
    else               pw["battery_charging"] = nullptr;
    pw["pmu"] = Pmu::model();          // "AXP192" / "AXP2101" / "none"
    pw["board"] = BOARD_NAME;
    pw["battery_v"] = b.volts;
    pw["battery_pct"] = b.percent;
  }
  doc["heap_free"]    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);   // internal RAM
  doc["heap_min_free"] = g_stats.heapMinFree;
  doc["psram_free"]   = ESP.getFreePsram();

  // Everything a soak run needs to read off a node it cannot reach a console
  // on: why it last restarted, how long that run lasted, and what it is
  // running out of. See Diag.h.
  {
    JsonObject dg = doc["diag"].to<JsonObject>();
    const Diag::Boot& b = Diag::boot();
    JsonObject bo = dg["boot"].to<JsonObject>();
    bo["count"]        = b.count;
    bo["reason"]       = b.reason;
    bo["reason_name"]  = b.reasonName;
    bo["clean"]        = b.clean;
    // Absent rather than zero when a power cut took the RTC domain with it:
    // "unknown" and "it ran for no time at all" are not the same answer.
    if (b.prevUptimeKnown) bo["prev_uptime_s"] = b.prevUptimeS;

    Diag::Heap h = Diag::heap();
    JsonObject hp = dg["heap"].to<JsonObject>();
    hp["free"]          = h.freeInternal;
    hp["min_free"]      = h.minFreeInternal;
    hp["largest_block"] = h.largestBlock;   // free minus this is the fragmentation
    hp["psram_free"]    = h.freePsram;

    Diag::TaskStack st[16];
    const size_t n = Diag::stacks(st, sizeof(st) / sizeof(st[0]));
    JsonObject sk = dg["stacks"].to<JsonObject>();
    for (size_t i = 0; i < n; i++)
      if (st[i].present) sk[st[i].name] = st[i].headroom;    // bytes never used
    const char* lowestName = nullptr;
    const uint32_t lowest = Diag::lowestHeadroom(&lowestName);
    dg["stack_lowest"]      = lowest;
    dg["stack_lowest_task"] = lowestName ? lowestName : "none";

    RnsTransport::Tables t = RnsTransport::tables();
    JsonObject tb = dg["tables"].to<JsonObject>();
    tb["paths"]          = t.paths;
    tb["links"]          = t.links;
    tb["links_active"]   = t.activeLinks;
    tb["links_pending"]  = t.pendingLinks;
    tb["destinations"]   = t.destinations;
    tb["announces"]      = t.announces;
    tb["announces_held"] = t.heldAnnounces;
    tb["rates"]          = t.rates;
  }

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
  // The total stays, so anything already reading it keeps working; the
  // breakdown beside it is what says which of five different things happened.
  radio["rx_dropped"]              = g_stats.loraRxDropRing + g_stats.loraRxDropReasm +
                                     g_stats.loraRxDropPartial;
  radio["rx_dropped_ring"]         = g_stats.loraRxDropRing;
  radio["rx_dropped_reassembly"]   = g_stats.loraRxDropReasm;
  radio["rx_dropped_partial"]      = g_stats.loraRxDropPartial;
  radio["rx_crc_errors"]           = g_stats.loraRxCrcErrors;
  radio["rx_bad_length"]           = g_stats.loraRxBadLength;
  radio["rx_spurious_irq"]         = g_stats.loraRxSpuriousIrq;

  radio["beacon_interval"] = rs.beaconInterval;
  radio["callsign"]   = loraRadio.callsign();
  radio["beacons_tx"] = g_stats.beaconsTx;
  radio["beacons_rx"] = g_stats.beaconsRx;
  radio["announce_interval"] = rs.announceInterval;
  radio["announces_tx"] = g_stats.announcesTx;
  radio["announces_rx"] = g_stats.announcesRx;

  JsonObject peers    = doc["peers"].to<JsonObject>();
  peers["rns_tcp"]    = g_stats.tcpClients;      // Reticulum clients on :4242
  peers["wifi_sta"]   = WiFi.softAPgetStationNum();
  peers["tcp_rx_packets"] = g_stats.tcpRxPackets;
  doc["wifi_enabled"] = wifiEnabled();

  // Every way a host can reach this node, in one vocabulary (LocalLink.h):
  // the Wi-Fi access point and station, and the USB and PPP links this board
  // could carry — listed even when this build cannot run them, with the
  // reason, so a page never has to guess why a switch is missing.
  {
    JsonArray links = doc["local_links"].to<JsonArray>();
    for (size_t i = 0; i < LocalLink::count(); i++) {
      const LocalLink::Link* l = LocalLink::at(i);
      const LocalLink::Snapshot sn = l->snapshot();
      JsonObject o = links.add<JsonObject>();
      o["name"]       = l->name();
      o["type"]       = LocalLink::typeName(sn.type);
      o["hardware"]   = l->hardware();
      o["firmware"]   = l->firmware();
      o["enabled"]    = l->enabled();
      o["phase"]      = LocalLink::phaseName(sn.phase);
      o["up"]         = sn.phase == LocalLink::Phase::Ready;
      // JsonString copies. ArduinoJson stores a const char array by address —
      // it reads as a string literal, which lives for ever — and sn is a
      // local that dies at the end of this iteration, long before the
      // document is serialised. What went out as the address was whatever the
      // stack held by then.
      o["ip"]         = JsonString(sn.ip);
      o["addressing"] = LocalLink::addressingName(sn.addressing);
      o["uptime_s"]   = sn.uptimeS;
      // Absent, not null, where the link cannot count its hosts: the page
      // treats the two the same, and this document is polled every two
      // seconds by every open tab.
      if (sn.clientKnown) o["clients"] = sn.clients;
      if (l->reason()[0]) o["reason"] = l->reason();
    }
  }

#if HAS_SD
  {
    SdCard::Info si = sdCard.info();
    JsonObject sd = doc["sd"].to<JsonObject>();
    sd["state"]        = SdCard::stateName(si.state);
    sd["type"]         = si.type == CARD_SDHC ? "SDHC" : si.type == CARD_SD ? "SD" : si.type == CARD_MMC ? "MMC" : "";
    sd["card_bytes"]   = si.cardBytes;
    sd["volume_bytes"] = si.volumeBytes;
    sd["used_bytes"]   = si.usedBytes;
    sd["last_format"]  = si.lastFormat;
    sd["reserved"]     = sdCard.reserved();      // Reticulum store lives here
    sd["storage_lost"] = sdCard.storageLost();   // ... and the card was pulled
    // Ownership, so the page can offer the right action rather than every
    // action: a blank card can be taken, one of ours is already home or can be
    // taken back, and one belonging to another node is not ours to touch.
    // One read, from the copy the card task keeps — this handler runs on the
    // AsyncTCP task, which has no business opening files on the SD bus, and
    // certainly not once per field.
    const StoreHome::Ownership own = StoreHome::ownership();
    sd["card"]         = StoreHome::cardName(own.card);
    sd["store_home"]   = StoreHome::whereName(StoreHome::where());
    sd["migration"]    = StoreHome::lastResult();
    sd["migrating"]    = StoreHome::busy();
    // Whether each move can be offered is the node's answer and not the page's
    // to work out: "the store is on the card" stays true after the card has
    // been pulled, and a page reasoning from that alone offered an eject that
    // cost a restart and then had no card to read the store off.
    sd["can_adopt"]    = StoreHome::canAdopt();
    sd["can_eject"]    = StoreHome::canEject();
    // JsonString, which copies, and not the bare array. ArduinoJson stores a
    // const char array by address — it reads as a string literal, which lives
    // for ever — and this one is a local that goes out of scope with the block,
    // some eighty lines of document-building before any of it is serialised.
    // What left the node as the owner's name was whatever the stack held by
    // then. The neighbouring fields survived only because they are copied from
    // non-const arrays, which is not a distinction to leave anything resting on.
    if (own.owner[0]) {
      sd["owner"]      = JsonString(own.owner);
      sd["generation"] = own.generation;
    }
  }
#endif

  {
    // Channel use and the hourly transmit budget (see Airtime.h)
    JsonObject at = doc["airtime"].to<JsonObject>();
    at["short_pct"]     = roundf(g_stats.airtimeShort * 10000.0f) / 100.0f;
    at["long_pct"]      = roundf(g_stats.airtimeLong * 10000.0f) / 100.0f;
    const Airtime::Band* band = Airtime::bandFor(settings.radio().freqMhz, settings.radio().bwKhz);
    // A node in the US band or at 2.4 GHz is not an exception to the European
    // plan, it is under a different one — reporting it as "outside the EU
    // 863-870 plan" described the only regime this field knows rather than the
    // regime the node is in. The sub-band figures below stay EU-specific
    // because only that plan has sub-bands to report.
    const Airtime::RegionInfo* areg =
      Airtime::regionFor(settings.radio().region, settings.radio().freqMhz);
    at["band"]           = band ? band->name
                          : (areg->regime == Airtime::Regime::EuSrd868
                             ? "outside the EU 863-870 plan" : areg->name);
    at["regime"]         = Airtime::regimeName(areg->regime);
    at["band_limit_pct"] = band ? band->basisPoints / 100.0f : 0.0f;
    at["band_allocated"] = band ? band->allocated : false;
    at["duty_limit_pct"] = g_stats.dutyLimitBp / 100.0f;        // what is enforced
    at["duty_manual_pct"] = settings.radio().dutyCyclePct;      // 0 = follow the band
    at["budget_used"]   = roundf(g_stats.dutyBudget * 1000.0f) / 1000.0f;
    at["locked"]        = g_stats.dutyLocked;
    at["retry_after_s"] = g_stats.dutyRetryS;
    at["csma_slot_ms"]  = g_stats.csmaSlotMs;
    at["csma_band"]     = g_stats.csmaBand;
  }

#if HAS_GPS
  {
    Gps::Fix g = Gps::fix();
    JsonObject gps = doc["gps"].to<JsonObject>();
    gps["enabled"]    = g.enabled;
    gps["fix"]        = g.valid;
    gps["quality"]    = g.quality;
    gps["satellites"] = g.satellites;
    gps["sentences"]  = g.sentences;
    gps["clock_set"]  = g.clockSet;
    gps["utc"]        = g.utc;
    // Everything above says whether the receiver is working. Where the node
    // physically is says something else, and /api/status is public — on an
    // open access point that is anyone within radio range. Coordinates are
    // therefore withheld unless the operator has published them, or the
    // caller holds the admin credentials.
    const bool sharePosition = settings.radio().gpsSharePosition ||
                               request->authenticate(ADMIN_USER, settings.admin().password);
    gps["position_public"] = settings.radio().gpsSharePosition;
    // HDOP says how well the receiver is solving, not where it is, so it goes
    // out with the rest of the health readings. It used to be published only
    // alongside the coordinates, which left it missing on the default private
    // configuration — and any consumer assuming a fix implies an HDOP broke
    // there and nowhere else.
    if (g.valid) gps["hdop"] = g.hdop;
    if (g.valid && sharePosition) {
      gps["latitude"]   = g.latitude;
      gps["longitude"]  = g.longitude;
      gps["altitude_m"] = g.altitude;
      gps["speed_kmh"]  = g.speedKmh;
    }
  }
#endif

  {
    JsonObject st = doc["storage"].to<JsonObject>();
    st["backend"] = RnsTransport::storageBackend();     // "sd" | "littlefs"
    st["path"]    = RnsTransport::storagePath();
    st["lost"]    = sdCard.storageLost();
    // Whether a card in the slot could take the store is published once, above,
    // as sd.can_adopt. It used to be worked out a second time here — the same
    // rule in two places, and the dashboard's copy went on saying a card was
    // free while a move onto it was already queued.
  }

  // Reticulum transport: interfaces with their modes, and the path table
  JsonObject tr = doc["transport"].to<JsonObject>();
  tr["enabled"] = settings.transport().enabled;
  tr["online"]  = g_stats.transportOnline;
  tr["lora_mode"] = RnsTransport::modeName(settings.transport().loraMode);
  tr["wifi_mode"] = RnsTransport::modeName(settings.transport().wifiMode);
  JsonObject ai = tr["autointerface"].to<JsonObject>();
  ai["enabled"] = settings.transport().autoEnabled;
  ai["online"]  = AutoInterface::enabled();
  ai["address"] = AutoInterface::localAddress();
  ai["peers"]   = AutoInterface::peerCount();
  ai["group_id"] = settings.transport().autoGroupId[0] ? settings.transport().autoGroupId : AUTOIF_GROUP_ID;
  {
    RnsTransport::IfaceInfo ifs[RNS_MAX_CLIENTS + 1];
    size_t k = RnsTransport::interfaces(ifs, RNS_MAX_CLIENTS + 1);
    JsonArray ia = tr["interfaces"].to<JsonArray>();
    for (size_t i = 0; i < k; i++) {
      JsonObject o = ia.add<JsonObject>();
      o["name"] = ifs[i].name; o["mode"] = ifs[i].mode; o["rx_bytes"] = ifs[i].rxb; o["tx_bytes"] = ifs[i].txb;
    }
    RnsTransport::PathInfo ps[32];
    size_t pk = RnsTransport::paths(ps, 32);
    tr["path_count"] = RnsTransport::pathCount();
    JsonArray pa = tr["paths"].to<JsonArray>();
    for (size_t i = 0; i < pk; i++) {
      JsonObject o = pa.add<JsonObject>();
      o["hash"] = ps[i].hash; o["hops"] = ps[i].hops; o["via"] = ps[i].via; o["age_s"] = ps[i].ageS;
    }
  }

  // Stations heard on the channel (beacons / RNode station IDs)
  Neighbor snap[MAX_NEIGHBORS];
  size_t n = neighbors.snapshot(snap, MAX_NEIGHBORS);
  JsonArray nb = doc["neighbors"].to<JsonArray>();
  uint32_t now = millis();
  for (size_t i = 0; i < n; i++) {
    JsonObject o = nb.add<JsonObject>();
    o["name"]    = snap[i].name;
    o["version"] = snap[i].version;
    o["kind"]    = snap[i].kind == NeighborKind::Announce ? "announce"
                 : snap[i].kind == NeighborKind::Beacon   ? "beacon" : "station-id";
    o["hash"]    = snap[i].hash;
    o["aspect"]  = snap[i].aspect;
    o["hops"]    = snap[i].hops;
    o["via"]     = snap[i].viaWifi ? "wifi" : "lora";
    o["rssi"]    = snap[i].rssi;
    o["snr"]     = snap[i].snr;
    o["age_s"]   = (now - snap[i].lastSeen) / 1000;
    o["count"]   = snap[i].count;
  }

  sendJson(request, 200, doc);
}

// ---------------------------------------------------------------------------
// Bulletin board — deliberately public and unencrypted; lives on this node
// only. Private traffic belongs on Reticulum, which this node cannot read.
// ---------------------------------------------------------------------------
// LittleFS.exists() and open() log a VFS error line for a file that is not
// there, and the board is empty until someone posts — so every status poll
// printed an error. stat() answers the same question silently.
void WifiManager::deriveHostname() {
  Mdns::label(_ssid, _hostname, sizeof(_hostname), MDNS_HOSTNAME);
}

static bool littleFsHas(const char* path) {
  struct stat st;
  return stat((String("/littlefs") + path).c_str(), &st) == 0;
}

// GET /api/qr?what=wifi|portal|address -> image/svg+xml
void WifiManager::handleQrFor(AsyncWebServerRequest* request, Qr::Payload what) {
  char text[192];
  if (!Qr::payloadText(what, text, sizeof(text))) { sendError(request, 503, "nothing to encode: no link is up, or the payload does not fit"); return; }
  QRCode qr;
  uint8_t buffer[Qr::MAX_BUFFER];
  if (!Qr::encode(text, qr, buffer)) { sendError(request, 500, "does not fit in a QR code"); return; }
  AsyncWebServerResponse* res = request->beginResponse(200, "image/svg+xml", Qr::toSvg(qr));
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

void WifiManager::handleBoardGet(AsyncWebServerRequest* request) {
  String out = "[]";
  if (littleFsHas(BOARD_FILE)) {
    File f = LittleFS.open(BOARD_FILE, "r");
    if (f) { out = f.readString(); f.close(); }
  }
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
  if (littleFsHas(BOARD_FILE)) {
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
  radio["region"]    = rs.region;
  // What this particular transceiver can be asked for. The settings page used
  // to offer the sub-GHz bandwidth steps to every board, which on a 2.4 GHz
  // radio is a list of values it cannot tune to.
  {
    const RadioCaps::Caps& c = loraRadio.caps();
    JsonObject cp = radio["caps"].to<JsonObject>();
    cp["model"]        = c.name;
    cp["freq_min_mhz"] = c.freqMinMhz;
    cp["freq_max_mhz"] = c.freqMaxMhz;
    cp["sf_min"]       = c.sfMin;
    cp["sf_max"]       = c.sfMax;
    cp["tx_min_dbm"]   = c.txMinDbm;
    cp["tx_max_dbm"]   = c.txMaxDbm;
    // An amplifier does not change what the chip may be driven at, but it does
    // change what leaves the antenna, and the operator has to account for it.
    cp["pa_fitted"]    = LoRaRadio::hasPa();
    JsonArray bws = cp["bandwidths_khz"].to<JsonArray>();
    for (const float* b = c.bandwidthsKhz; *b != 0.0f; b++) bws.add(*b);
    // Which rulebook the configured channel falls under, and what it caps
    // What this node will actually enforce, which is decided by its region —
    // reporting the frequency's regime here told an operator on "custom" that
    // the EU plan applied while the radio had already stopped applying it.
    const Airtime::Regime rg =
      Airtime::regionFor(settings.radio().region, settings.radio().freqMhz)->regime;
    cp["regime"]       = Airtime::regimeName(rg);
    cp["max_dwell_ms"] = Airtime::maxDwellMs(rg);        // 0 = not a dwell regime

    // Only the regions this radio can actually reach. Offering "Europe
    // 863-870" on a 2.4 GHz node would be a choice that cannot be honoured,
    // and the operator would find that out only when the frequency was
    // rejected. Custom is always offered: it is the escape hatch.
    JsonArray regs = cp["regions"].to<JsonArray>();
    size_t n = 0;
    const Airtime::RegionInfo* all = Airtime::regions(n);
    for (size_t i = 0; i < n; i++) {
      const Airtime::RegionInfo& ri = all[i];
      const bool custom = (ri.id == Airtime::Region::Custom);
      // Reachable when the region's band and the chip's tuning range overlap
      const bool reachable = custom ||
        (ri.highMhz >= c.freqMinMhz && ri.lowMhz <= c.freqMaxMhz);
      if (!reachable) continue;
      JsonObject o = regs.add<JsonObject>();
      o["key"]       = ri.key;
      o["name"]      = ri.name;
      o["low_mhz"]   = custom ? c.freqMinMhz : max(ri.lowMhz,  c.freqMinMhz);
      o["high_mhz"]  = custom ? c.freqMaxMhz : min(ri.highMhz, c.freqMaxMhz);
      o["regime"]    = Airtime::regimeName(ri.regime);
      o["dwell_ms"]  = Airtime::maxDwellMs(ri.regime);
      // Custom carries no channel of its own — it is offered on every radio,
      // so a fixed sub-GHz suggestion would be untunable on a 2.4 GHz one.
      // Fall back to the middle of what this chip can reach and its widest
      // bandwidth, which is at least always a valid starting point.
      float dfl = ri.defaultMhz, dbw = ri.defaultBwKhz;
      if (dfl == 0.0f) dfl = (c.freqMinMhz + c.freqMaxMhz) / 2.0f;
      if (dbw == 0.0f) {
        dbw = c.bandwidthsKhz[0];
        for (const float* b = c.bandwidthsKhz; *b != 0.0f; b++) dbw = *b;
      }
      o["default_mhz"] = dfl;
      o["default_bw_khz"] = dbw;
      o["default_sf"]  = ri.defaultSf;
    }
  }
  radio["sync_word"] = rs.syncWord;
  radio["preamble"]  = rs.preamble;
  radio["beacon_interval"] = rs.beaconInterval;
  radio["announce_interval"] = rs.announceInterval;
  radio["callsign"]  = rs.callsign;              // "" = SSID
  radio["duty_cycle_pct"] = rs.dutyCyclePct;     // manual cap; 0 = follow the band
  radio["gps_enabled"] = rs.gpsEnabled;
  radio["gps_share_position"] = rs.gpsSharePosition;
  radio["has_gps"] = HAS_GPS ? true : false;
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
  wifi["sta_ssid"]   = ws.staSsid;
  wifi["sta_has_password"] = ws.staPassword[0] != '\0';
  wifi["sta_connected"] = stationConnected();

  JsonObject tr = doc["transport"].to<JsonObject>();
  tr["enabled"]   = settings.transport().enabled;
  tr["lora_mode"] = settings.transport().loraMode;
  tr["wifi_mode"] = settings.transport().wifiMode;
  tr["announce_cap"] = settings.transport().announceCap;
  tr["announce_rate_target"] = settings.transport().announceRateTarget;
  tr["announce_rate_grace"] = settings.transport().announceRateGrace;
  tr["announce_rate_penalty"] = settings.transport().announceRatePenalty;
  tr["auto_enabled"] = settings.transport().autoEnabled;
  tr["auto_group_id"] = settings.transport().autoGroupId;
  tr["power_profile"] = Power::profileName((Power::Profile)settings.transport().powerProfile);
  tr["sd_store"] = settings.transport().sdStore;
  tr["online"]    = g_stats.transportOnline;

  // Which local links exist, which this build can run, and which are on —
  // three answers per link, because the page has to show a switch that is
  // off, a switch that cannot be turned on, and no switch as different things.
  {
    JsonObject links = doc["links"].to<JsonObject>();
    size_t n = 0;
    const LocalLink::Field* f = LocalLink::fields(n);
    for (size_t i = 0; i < n; i++) {
      const LocalLink::Link* l = LocalLink::find(f[i].type);
      if (!l) continue;
      JsonObject o = links[f[i].key].to<JsonObject>();
      o["hardware"]  = l->hardware();
      o["supported"] = l->usable();
      o["enabled"]   = LocalLink::switchOn(*l, settings.links());
      if (l->reason()[0]) o["reason"] = l->reason();
    }
  }
  {
    JsonObject m = doc["maintenance"].to<JsonObject>();
    for (const MaintField& f : kMaintFields) m[f.key] = settings.maintenance().*(f.on);
  }
  bootloaderJson(doc["bootloader"].to<JsonObject>());

  doc["admin"]["user"] = ADMIN_USER;
  doc["admin"]["default_password"] = strcmp(settings.admin().password, ADMIN_PASSWORD_DEFAULT) == 0;

  sendJson(request, 200, doc);
}

// ---------------------------------------------------------------------------
// Local links and maintenance settings
// ---------------------------------------------------------------------------
// POST /api/settings/links {"wifi":bool,"usb":bool,"ppp":bool} — any subset.
// A link the board lacks or the build cannot run is refused by name rather
// than saved: a setting nothing acts on is a lie the page would go on
// showing. Wi-Fi changes restart the node (the AP cannot be torn down under
// the request that asked); the answer says so.
void WifiManager::handleLinksPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }
  LinkSettings want = settings.links();
  bool changed[8] = {};
  size_t n = 0;
  const LocalLink::Field* f = LocalLink::fields(n);
  for (size_t i = 0; i < n && i < 8; i++) {
    if (!in[f[i].key].is<bool>()) continue;
    want.*(f[i].on) = in[f[i].key];
    changed[i] = true;
  }
  const char* detail = "";
  const LocalLink::Apply a = LocalLink::applyLinks(want, changed, Bootloader::Source::Settings, &detail);
  JsonDocument out;
  switch (a) {
    case LocalLink::Apply::RefusedUnusable: {
      char msg[160];
      snprintf(msg, sizeof(msg), "cannot be enabled: %s", detail);
      sendError(request, 400, msg); return;
    }
    case LocalLink::Apply::RefusedLockedOut:
      // Turning every host-facing link off is allowed — the console is the
      // way back — unless the console is off too, in which case there is no
      // way back short of erasing the flash. Refused, not warned about.
      sendError(request, 400, "refused: with the serial console switched off this would leave no way to reach the node; turn the console on first"); return;
    case LocalLink::Apply::RefusedBusy:  sendError(request, 503, kRestartingMsg); return;
    case LocalLink::Apply::NvsFailed:    sendError(request, 500, "nvs"); return;
    case LocalLink::Apply::Unchanged:
    case LocalLink::Apply::Saved:
    case LocalLink::Apply::SavedRestarting:
    case LocalLink::Apply::SavedNextBoot:
      break;
  }
  out["ok"] = true;
  out["restart"] = a == LocalLink::Apply::SavedRestarting;
  if (a == LocalLink::Apply::SavedNextBoot)
    out["note"] = "saved; a restart is already in progress, so the change applies at the next boot";
  else if (!LocalLink::anySwitchOn(want))
    out["note"] = "no local link is enabled; the node answers only on the serial maintenance console (WIFI ON restores the access point)";
  sendJson(request, 200, out);
}

// POST /api/settings/maintenance {"bootloader_api":bool,"bootloader_from_lan":bool,"console_enabled":bool}
void WifiManager::handleMaintenancePost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }
  MaintenanceSettings m = settings.maintenance();
  for (const MaintField& f : kMaintFields)
    if (in[f.key].is<bool>()) m.*(f.on) = in[f.key];
  if (LocalLink::lockedOut(settings.links(), m.consoleEnabled)) {
    sendError(request, 400, "refused: no local link is enabled, so switching the console off would leave no way to reach the node");
    return;
  }
  if (!settings.saveMaintenance(m)) { sendError(request, 500, "nvs"); return; }
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// System: bootloader and reboot
//
// Putting a deployed relay into its ROM downloader is the most privileged
// thing the API can do — the node stops routing until someone flashes it or
// power-cycles it — so it is guarded three ways: the admin password, the
// maintenance switch, and the link the request came over. By default only a
// directly attached link qualifies (the access point, USB, PPP); the station
// uplink is somebody's LAN and is refused unless bootloader_from_lan is set.
// Nothing here is reachable through Reticulum: the API is HTTP on lwIP, and
// the node's Reticulum destination carries no such request.
// ---------------------------------------------------------------------------
void WifiManager::handleBootloaderGet(AsyncWebServerRequest* request) {
  JsonDocument doc;
  bootloaderJson(doc.to<JsonObject>());
  doc["board"]   = BOARD_NAME;
  doc["confirm"] = "BOOTLOADER";
  // Whether *this* request would be allowed, so a tool can tell before it asks.
  doc["allowed_from_here"] = fromHostFacingLink(request) || settings.maintenance().bootloaderFromLan;
  sendJson(request, 200, doc);
}

// POST /api/system/bootloader {"confirm":"BOOTLOADER"} -> 202 and, 600 ms
// later, the ROM downloader. The reply carries what the tool needs next.
void WifiManager::handleBootloaderPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  if (!settings.maintenance().bootloaderApi) {
    sendError(request, 403, "the bootloader API is switched off in maintenance settings"); return;
  }
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "BOOTLOADER") != 0) {
    sendError(request, 400, "send {\"confirm\":\"BOOTLOADER\"} to restart into the ROM downloader"); return;
  }
  if (!fromHostFacingLink(request) && !settings.maintenance().bootloaderFromLan) {
    sendError(request, 403, "only from a directly attached link (access point, USB, PPP); "
                            "set bootloader_from_lan to allow it from the station network"); return;
  }
  const char* why = nullptr;
  const Bootloader::Refusal r = Bootloader::request(Bootloader::Target::Bootloader, Bootloader::Source::Http, RESTART_ACK_DELAY_MS, &why);
  if (r != Bootloader::Refusal::None) { sendError(request, Bootloader::httpStatus(r), why); return; }
  JsonDocument out;
  out["ok"] = true;
  out["restart"] = true;
  out["target"] = "bootloader";
  out["method"] = Bootloader::methodName(Bootloader::Method::SoftwareApi);
  out["delay_ms"] = RESTART_ACK_DELAY_MS;
  #if BOARD_USB_NATIVE
    out["expect"] = "USB-Serial/JTAG device 303a:1001 in download mode";
  #else
    out["expect"] = "ROM downloader on UART0 behind the " BOARD_USB_BRIDGE " bridge";
  #endif
  out["recovery"] = Bootloader::manualRecovery();
  sendJson(request, 202, out);
}

// POST /api/system/reboot {"confirm":"REBOOT"} -> 202, then a plain restart.
void WifiManager::handleRebootPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "REBOOT") != 0) {
    sendError(request, 400, "send {\"confirm\":\"REBOOT\"} to restart the node"); return;
  }
  const char* why = nullptr;
  const Bootloader::Refusal r = Bootloader::request(Bootloader::Target::App, Bootloader::Source::Http, RESTART_ACK_DELAY_MS, &why);
  if (r != Bootloader::Refusal::None) { sendError(request, Bootloader::httpStatus(r), why); return; }
  request->send(202, "application/json", "{\"ok\":true,\"restart\":true,\"target\":\"app\"}");
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
  if (in["announce_interval"].is<int>()) r.announceInterval = in["announce_interval"];
  if (in["duty_cycle_pct"].is<int>()) r.dutyCyclePct = in["duty_cycle_pct"];
  if (in["gps_enabled"].is<bool>())   r.gpsEnabled   = in["gps_enabled"];
  if (in["gps_share_position"].is<bool>()) r.gpsSharePosition = in["gps_share_position"];
  if (in["callsign"].is<const char*>()) {
    String c = in["callsign"].as<String>(); c.trim();
    for (size_t i = 0; i < c.length(); i++)
      if (c[i] < 0x21 || c[i] > 0x7E) { sendError(request, 400, "callsign: printable ASCII without spaces only"); return; }
    if (c.length() > 32) { sendError(request, 400, "callsign must be at most 32 characters"); return; }
    strlcpy(r.callsign, c.c_str(), sizeof(r.callsign));
  }
  if (in["region"].is<const char*>()) {
    strlcpy(r.region, in["region"].as<const char*>(), sizeof(r.region));
  }

  int8_t maxDbm = loraRadio.online() ? loraRadio.maxTxDbm() : 22;
  // Bounds come from the transceiver that is actually fitted, not from a
  // sub-GHz assumption: an SX1280 tunes 2400-2500 MHz and has four bandwidths,
  // none of which appear in the SX127x list.
  const RadioCaps::Caps& caps = loraRadio.caps();
  char msg[160], bwlist[96];

  // The region bounds the channel, and the chip bounds the region. Both have
  // to hold, and the message says which one was missed rather than quoting a
  // range the operator cannot use anyway.
  const Airtime::RegionInfo* region = Airtime::regionByKey(r.region);
  if (!region) { sendError(request, 400, "unknown region — pick one the node offers"); return; }
  const float lowMhz  = max(region->lowMhz,  caps.freqMinMhz);
  const float highMhz = min(region->highMhz, caps.freqMaxMhz);
  if (lowMhz > highMhz) {
    snprintf(msg, sizeof(msg), "the %s cannot tune %s — choose a region this radio reaches",
             caps.name, region->name);
    sendError(request, 400, msg); return;
  }
  if (r.freqMhz < lowMhz || r.freqMhz > highMhz) {
    snprintf(msg, sizeof(msg), "frequency must be %g-%g MHz in %s on the %s",
             (double)lowMhz, (double)highMhz, region->name, caps.name);
    sendError(request, 400, msg); return;
  }
  if (!RadioCaps::bandwidthSupported(caps, r.bwKhz)) {
    snprintf(msg, sizeof(msg), "the %s supports these bandwidths in kHz: %s",
             caps.name, RadioCaps::bandwidthList(caps, bwlist, sizeof(bwlist)));
    sendError(request, 400, msg); return;
  }
  if (r.sf < caps.sfMin || r.sf > caps.sfMax) {
    snprintf(msg, sizeof(msg), "spreading factor must be %u-%u on the %s",
             (unsigned)caps.sfMin, (unsigned)caps.sfMax, caps.name);
    sendError(request, 400, msg); return;
  }
  if (r.cr < 5 || r.cr > 8)                        { sendError(request, 400, "coding rate must be 5-8 (4/5..4/8)"); return; }
  if (r.txDbm < caps.txMinDbm || r.txDbm > maxDbm) {
    snprintf(msg, sizeof(msg), "tx power must be %d to %d dBm on the %s",
             (int)caps.txMinDbm, (int)maxDbm, caps.name);
    sendError(request, 400, msg); return;
  }
  if (r.preamble < 6 || r.preamble > 1000)         { sendError(request, 400, "preamble must be 6-1000 symbols"); return; }
  if (r.beaconInterval != 0 && (r.beaconInterval < 10 || r.beaconInterval > 3600)) { sendError(request, 400, "beacon interval must be 0 (off) or 10-3600 s"); return; }
  if (r.announceInterval != 0 && (r.announceInterval < 60 || r.announceInterval > 43200)) { sendError(request, 400, "announce interval must be 0 (off) or 60-43200 s"); return; }
  if (r.dutyCyclePct > 100)                        { sendError(request, 400, "duty cycle must be 0 (off) or 1-100 %"); return; }

  if (!settings.saveRadio(r)) { sendError(request, 500, "nvs"); return; }
  if (loraRadio.online()) loraRadio.requestReconfigure(r);
#if HAS_GPS
  Gps::setEnabled(r.gpsEnabled);                 // applies without a restart
#endif

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
  if (in["sta_ssid"].is<const char*>()) {
    String s = in["sta_ssid"].as<String>(); s.trim();
    if (s.length() > 32) { sendError(request, 400, "station ssid must be at most 32 characters"); return; }
    strlcpy(w.staSsid, s.c_str(), sizeof(w.staSsid));
    if (s.isEmpty()) w.staPassword[0] = '\0';
  }
  if (in["sta_password"].is<const char*>()) {
    const char* p = in["sta_password"];
    if (p[0] != '\0') {                    // empty = keep
      if (strlen(p) > 63) { sendError(request, 400, "station password too long"); return; }
      strlcpy(w.staPassword, p, sizeof(w.staPassword));
    }
  }

  if (w.security != ApSecurity::Open && strlen(w.password) < 8) { sendError(request, 400, "a password is required for a secured network"); return; }
  if (w.channel < 1 || w.channel > 13)          { sendError(request, 400, "channel must be 1-13"); return; }
  if (w.maxStations < 1 || w.maxStations > 10)  { sendError(request, 400, "max stations must be 1-10"); return; }

  if (!settings.saveWifi(w)) { sendError(request, 500, "nvs"); return; }

  JsonDocument out;
  out["ok"] = true;
  out["restart"] = true;
  out["ssid"] = w.ssid[0] ? w.ssid : _ssid;   // auto-derived name does not change
  out["security"] = Settings::securityName(w.security);
  // Asked before the answer is sent, so the answer can say whether it was
  // granted; the delay is what lets the reply leave, not the order here.
  out["restart"] = Bootloader::reboot();
  sendJson(request, 200, out);
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

void WifiManager::handleTransportPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }
  TransportSettings t = settings.transport();
  if (in["enabled"].is<bool>())  t.enabled  = in["enabled"];
  if (in["lora_mode"].is<int>()) t.loraMode = in["lora_mode"];
  if (in["wifi_mode"].is<int>()) t.wifiMode = in["wifi_mode"];
  if (in["announce_cap"].is<int>())          t.announceCap         = in["announce_cap"];
  if (in["announce_rate_target"].is<int>())  t.announceRateTarget  = in["announce_rate_target"];
  if (in["announce_rate_grace"].is<int>())   t.announceRateGrace   = in["announce_rate_grace"];
  if (in["announce_rate_penalty"].is<int>()) t.announceRatePenalty = in["announce_rate_penalty"];
  if (in["auto_enabled"].is<bool>()) t.autoEnabled = in["auto_enabled"];
  // Where the store lives is not a field you can save. It used to be: this
  // wrote the new value to NVS and restarted, and the node came up pointed at
  // a filesystem the data had never been copied to — an empty path table, with
  // the real one still sitting on the other side. Moving it is a copy, and the
  // copy is what adopt and eject do.
  if (in["sd_store"].is<bool>() && (bool)in["sd_store"] != t.sdStore) {
    sendError(request, 409, "the store is moved with the SD card actions (Use this card / Eject), not by saving this form");
    return;
  }
  if (in["power_profile"].is<const char*>()) {
    Power::Profile pp;
    if (!Power::profileFromName(in["power_profile"], pp)) { sendError(request, 400, "power_profile must be performance|balanced|battery"); return; }
    t.powerProfile = (uint8_t)pp;
  }
  if (in["auto_group_id"].is<const char*>()) {
    String g = in["auto_group_id"].as<String>(); g.trim();
    if (g.length() > 32) { sendError(request, 400, "group id must be at most 32 characters"); return; }
    strlcpy(t.autoGroupId, g.c_str(), sizeof(t.autoGroupId));
  }
  if (t.loraMode < 1 || t.loraMode > 5 || t.wifiMode < 1 || t.wifiMode > 5) { sendError(request, 400, "mode must be 1-5"); return; }
  if (t.announceCap < 1 || t.announceCap > 100) { sendError(request, 400, "announce cap must be 1-100 %"); return; }
  TransportSettings before = settings.transport();
  if (!settings.saveTransport(t)) { sendError(request, 500, "nvs"); return; }
  Power::apply((Power::Profile)t.powerProfile);                  // live
  bool needRestart = before.enabled != t.enabled || before.loraMode != t.loraMode || before.wifiMode != t.wifiMode
                  || before.autoEnabled != t.autoEnabled || strcmp(before.autoGroupId, t.autoGroupId) != 0
                  || before.announceCap != t.announceCap;
  JsonDocument out;
  out["ok"] = true;
  out["restart"] = needRestart && Bootloader::reboot();   // interfaces are registered at boot
  sendJson(request, 200, out);
}

// GET /api/settings/export — everything needed to clone a node's
// configuration (identity keys are deliberately NOT included).
void WifiManager::handleExport(AsyncWebServerRequest* request) {
  const RadioSettings& rs = settings.radio();
  const WifiSettings&  ws = settings.wifi();
  const TransportSettings& ts = settings.transport();
  JsonDocument doc;
  doc["retimesh_settings"] = 1;                 // schema version
  doc["firmware"] = FW_VERSION;
  JsonObject r = doc["radio"].to<JsonObject>();
  r["freq_mhz"] = rs.freqMhz; r["bw_khz"] = rs.bwKhz; r["sf"] = rs.sf; r["cr"] = rs.cr; r["tx_dbm"] = rs.txDbm;
  r["sync_word"] = rs.syncWord; r["preamble"] = rs.preamble; r["announce_interval"] = rs.announceInterval;
  r["beacon_interval"] = rs.beaconInterval; r["callsign"] = rs.callsign;
  r["duty_cycle_pct"] = rs.dutyCyclePct; r["region"] = rs.region;
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["ssid"] = ws.ssid; w["security"] = Settings::securityName(ws.security); w["password"] = ws.password;
  w["channel"] = ws.channel; w["max_stations"] = ws.maxStations; w["hidden"] = ws.hidden;
  w["sta_ssid"] = ws.staSsid; w["sta_password"] = ws.staPassword;
  JsonObject t = doc["transport"].to<JsonObject>();
  t["enabled"] = ts.enabled; t["lora_mode"] = ts.loraMode; t["wifi_mode"] = ts.wifiMode;
  t["announce_cap"] = ts.announceCap; t["announce_rate_target"] = ts.announceRateTarget;
  t["announce_rate_grace"] = ts.announceRateGrace; t["announce_rate_penalty"] = ts.announceRatePenalty;
  t["auto_enabled"] = ts.autoEnabled; t["auto_group_id"] = ts.autoGroupId;
  t["power_profile"] = ts.powerProfile; t["sd_store"] = ts.sdStore;
  {
    // Only the links this build can run: an export describes what the node
    // does, and a switch for a driver that does not exist here would carry a
    // meaningless value onto a node where it means something.
    JsonObject l = doc["links"].to<JsonObject>();
    size_t n = 0;
    const LocalLink::Field* f = LocalLink::fields(n);
    for (size_t i = 0; i < n; i++) {
      const LocalLink::Link* link = LocalLink::find(f[i].type);
      if (link && link->usable()) l[f[i].key] = LocalLink::switchOn(*link, settings.links());
    }
  }
  JsonObject m = doc["maintenance"].to<JsonObject>();
  for (const MaintField& f : kMaintFields) m[f.key] = settings.maintenance().*(f.on);
  doc["admin"]["password"] = settings.admin().password;
  String out; serializeJsonPretty(doc, out);
  AsyncWebServerResponse* res = request->beginResponse(200, "application/json", out);
  res->addHeader("Content-Disposition", "attachment; filename=\"retimesh-settings.json\"");
  request->send(res);
}

// POST /api/settings/import — applies an export (sections are optional),
// then restarts. Same validation as the individual endpoints.
void WifiManager::handleImport(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || !in["retimesh_settings"].is<int>()) {
    sendError(request, 400, "not a RetiMesh settings export"); return;
  }
  if (in["radio"].is<JsonObject>()) {
    JsonObject r = in["radio"]; RadioSettings rs = settings.radio();
    rs.freqMhz = r["freq_mhz"] | rs.freqMhz; rs.bwKhz = r["bw_khz"] | rs.bwKhz; rs.sf = r["sf"] | rs.sf; rs.cr = r["cr"] | rs.cr;
    rs.txDbm = r["tx_dbm"] | rs.txDbm; rs.syncWord = r["sync_word"] | rs.syncWord; rs.preamble = r["preamble"] | rs.preamble;
    rs.announceInterval = r["announce_interval"] | rs.announceInterval; rs.beaconInterval = r["beacon_interval"] | rs.beaconInterval;
    if (r["callsign"].is<const char*>()) strlcpy(rs.callsign, r["callsign"], sizeof(rs.callsign));
    // Present but unknown is an error, as it is on the POST path. Absent means
    // a config exported before regions existed, and that is what the frequency
    // is for. Starting from the node's own region would have made a legacy
    // import silently inherit it, and a typo silently correct itself.
    const Airtime::RegionInfo* ireg = nullptr;
    if (r["region"].is<const char*>()) {
      ireg = Airtime::regionByKey(r["region"]);
      if (!ireg) { sendError(request, 400, "radio section names an unknown region"); return; }
    } else {
      ireg = Airtime::regionForFreq(rs.freqMhz);
    }
    // The same bounds the POST path applies, for the same reason: an import
    // used to be validated against hardcoded sub-GHz limits, so a 2.4 GHz node
    // could not restore its own export, and a sub-GHz one could import a
    // configuration the API would have refused — straight into NVS.
    const RadioCaps::Caps& icaps = loraRadio.caps();
    const float ilow  = max(ireg->lowMhz,  icaps.freqMinMhz);
    const float ihigh = min(ireg->highMhz, icaps.freqMaxMhz);
    if (rs.freqMhz < ilow || rs.freqMhz > ihigh ||
        !RadioCaps::bandwidthSupported(icaps, rs.bwKhz) ||
        rs.sf < icaps.sfMin || rs.sf > icaps.sfMax ||
        rs.cr < 5 || rs.cr > 8 ||
        rs.txDbm < icaps.txMinDbm || rs.txDbm > loraRadio.maxTxDbm()) {
      sendError(request, 400, "radio section invalid for the transceiver in this node"); return;
    }
    strlcpy(rs.region, ireg->key, sizeof(rs.region));
    settings.saveRadio(rs);
  }
  if (in["wifi"].is<JsonObject>()) {
    JsonObject w = in["wifi"]; WifiSettings ws = settings.wifi();
    if (w["ssid"].is<const char*>()) strlcpy(ws.ssid, w["ssid"], sizeof(ws.ssid));
    if (w["password"].is<const char*>()) strlcpy(ws.password, w["password"], sizeof(ws.password));
    if (w["security"].is<const char*>()) Settings::securityFromName(w["security"], ws.security);
    if (w["sta_ssid"].is<const char*>()) strlcpy(ws.staSsid, w["sta_ssid"], sizeof(ws.staSsid));
    if (w["sta_password"].is<const char*>()) strlcpy(ws.staPassword, w["sta_password"], sizeof(ws.staPassword));
    ws.channel = w["channel"] | ws.channel; ws.maxStations = w["max_stations"] | ws.maxStations; ws.hidden = w["hidden"] | ws.hidden;
    if (ws.security != ApSecurity::Open && strlen(ws.password) < 8) ws.security = ApSecurity::Open;
    if (ws.channel < 1 || ws.channel > 13 || ws.maxStations < 1 || ws.maxStations > 10) { sendError(request, 400, "wifi section invalid"); return; }
    settings.saveWifi(ws);
  }
  bool storeHomeIgnored = false;
  if (in["transport"].is<JsonObject>()) {
    JsonObject t = in["transport"]; TransportSettings ts = settings.transport();
    ts.enabled = t["enabled"] | ts.enabled; ts.loraMode = t["lora_mode"] | ts.loraMode; ts.wifiMode = t["wifi_mode"] | ts.wifiMode;
    ts.announceCap = t["announce_cap"] | ts.announceCap; ts.announceRateTarget = t["announce_rate_target"] | ts.announceRateTarget;
    ts.announceRateGrace = t["announce_rate_grace"] | ts.announceRateGrace; ts.announceRatePenalty = t["announce_rate_penalty"] | ts.announceRatePenalty;
    ts.autoEnabled = t["auto_enabled"] | ts.autoEnabled;
    // Not imported, and not a reason to refuse the file either. Where the store
    // lives describes the node the backup came from — whether that one had a
    // card in its slot — and not the configuration being restored. Restoring
    // onto a replacement node is exactly when this differs and exactly when
    // failing the whole import is least welcome, so the field is dropped and
    // the answer says so. The store is moved with the card actions, which copy
    // the data; setting the flag alone never did.
    storeHomeIgnored = t["sd_store"].is<bool>() && (bool)t["sd_store"] != ts.sdStore;
    ts.powerProfile = t["power_profile"] | ts.powerProfile;
    if (t["auto_group_id"].is<const char*>()) strlcpy(ts.autoGroupId, t["auto_group_id"], sizeof(ts.autoGroupId));
    if (ts.loraMode < 1 || ts.loraMode > 5 || ts.wifiMode < 1 || ts.wifiMode > 5 || ts.announceCap < 1 || ts.announceCap > 100) { sendError(request, 400, "transport section invalid"); return; }
    settings.saveTransport(ts);
  }
  {
    // Links and maintenance together, because the one rule that spans them —
    // a node must keep some way in — has to be checked on the pair before
    // either half is saved. A link this board cannot run is dropped rather
    // than refused: the file describes the node it came from, and restoring
    // a T3-S3's export onto a Heltec is the normal case.
    LinkSettings ls = settings.links();
    MaintenanceSettings ms = settings.maintenance();
    const bool haveLinks = in["links"].is<JsonObject>();
    const bool haveMaint = in["maintenance"].is<JsonObject>();
    if (haveLinks) {
      JsonObject lk = in["links"];
      size_t n = 0;
      const LocalLink::Field* f = LocalLink::fields(n);
      for (size_t i = 0; i < n; i++) {
        if (!lk[f[i].key].is<bool>()) continue;
        const bool want = lk[f[i].key];
        const LocalLink::Link* link = LocalLink::find(f[i].type);
        if (want && (!link || !link->usable())) continue;
        ls.*(f[i].on) = want;
      }
    }
    if (haveMaint) {
      JsonObject mt = in["maintenance"];
      for (const MaintField& f : kMaintFields) ms.*(f.on) = mt[f.key] | ms.*(f.on);
    }
    if ((haveLinks || haveMaint) && LocalLink::lockedOut(ls, ms.consoleEnabled)) {
      sendError(request, 400, "refused: this file would leave the node with every local link off and the console off, and no way back in");
      return;
    }
    if (haveLinks) settings.saveLinks(ls);
    if (haveMaint) settings.saveMaintenance(ms);
  }
  if (in["admin"]["password"].is<const char*>()) {
    const char* p = in["admin"]["password"];
    if (strlen(p) >= 4 && strlen(p) <= 32) settings.saveAdminPassword(p);
  }
  JsonDocument out;
  out["ok"] = true;
  out["restart"] = Bootloader::reboot();
  if (storeHomeIgnored) out["note"] = "the store's location was not imported; move it with the SD card actions";
  sendJson(request, 200, out);
}

// POST /api/settings/sd/format {"confirm":"FORMAT"} — wipes the whole card.
void WifiManager::handleSdFormat(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "FORMAT") != 0) {
    sendError(request, 400, "send {\"confirm\":\"FORMAT\"} to erase the card"); return;
  }
  // The reasons a format can be refused live in SdCard, which is the only thing
  // that knows all of them, and the request answers with the one that applied.
  // Asking it again for something to say was a second reading of a rule that
  // turns on a card and a queued move, either of which can change in between —
  // so the message could name a reason that no longer held, or come up empty.
  if (const char* why = sdCard.requestFormat()) { sendError(request, 409, why); return; }
  request->send(200, "application/json", "{\"ok\":true,\"formatting\":true}");
}

// POST /api/settings/sd/adopt — copy the store onto the card and restart into it.
void WifiManager::handleSdAdopt(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "ADOPT") != 0) {
    sendError(request, 400, "send {\"confirm\":\"ADOPT\"} to move the store onto the card"); return;
  }
  // Every reason an adopt can be refused — no card, already there, a card
  // belonging to another node — is StoreHome's to give, and it gives it in
  // lastResult(). This handler had its own copy of the foreign-card refusal,
  // which is one rule in two places and a message that can disagree with the
  // decision it explains.
  if (!StoreHome::requestAdopt()) { sendError(request, 409, StoreHome::lastResult()); return; }
  request->send(200, "application/json", "{\"ok\":true,\"migrating\":true,\"restart\":true}");
}

// POST /api/settings/sd/eject — copy the store back to internal flash, restart,
// and leave the card safe to pull.
void WifiManager::handleSdEject(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "EJECT") != 0) {
    sendError(request, 400, "send {\"confirm\":\"EJECT\"} to move the store off the card"); return;
  }
  if (!StoreHome::requestEject()) { sendError(request, 409, StoreHome::lastResult()); return; }
  request->send(200, "application/json", "{\"ok\":true,\"migrating\":true,\"restart\":true}");
}

void WifiManager::handleReset(AsyncWebServerRequest* request) {
  settings.factoryReset();
  JsonDocument out;
  out["ok"] = true;
  out["restart"] = Bootloader::reboot();
  sendJson(request, 200, out);
}
