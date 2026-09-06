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
#include <memory>
#include <new>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include "LoRaRadio.h"
#include "LoRaFem.h"
#include "Neighbors.h"
#include "RnsAnnounce.h"
#include "RnsTransport.h"
#include "LxmfInbox.h"
#include "RnsAdmin.h"
#include "Mdns.h"
#include "Diag.h"
#include "SettingsRules.h"
#include "SettingsFields.h"
#include "SdCard.h"
#include "StoreHome.h"
#include "OtaUpdate.h"
#include "AutoInterface.h"
#include "Power.h"
#include "LocalLink.h"
#include "PppUart.h"
#include "Bootloader.h"

WifiManager wifiManager;

// One answer for every write path that has to refuse while a restart is on
// its way: a write accepted now may or may not reach NVS before it, and the
// caller could not tell which.
static const char kRestartingMsg[] = "the node is restarting";

static const char PORTAL_URL[] = "http://10.42.0.1/";
static const char ADMIN_USER[] = "admin";

// How many times tick() re-tries the AP config patch after startAccessPoint()
// had it refused with ESP_ERR_WIFI_STATE (a station connect in flight). 15
// tries at the 2 s gate span ~30 s — one full station-watchdog reconnect
// period — so at least one attempt lands in the quiet gap between connects;
// bounded, so a driver refusing for some other reason cannot buy itself a
// permanent 2 s poll.
static const uint8_t kApPatchAttempts = 15;

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

// Whether the whole document actually reached `out`.
//
// ArduinoJson's String writer cannot be asked this directly. Its constructor
// assigns a null const char*, which on ESP32 is String::invalidate() and frees
// whatever the String held — so reserving room up front is undone before the
// first byte is written. Its bulk write() returns the length it was given
// whatever the individual writes did, and its destructor drops the result of
// the final flush(). A short heap therefore does not fail: concat() refuses,
// the appends are lost, and serializeJson still reports success.
//
// The length is the evidence that survives all of that. measureJson() says how
// long the document is, String::length() says how much of it arrived, and
// overflowed() covers the other half — a document the heap could not hold in
// the first place, which serialises cleanly and is simply missing values.
//
// This matters because the failure is silent and looks like ours: about two in
// five status polls on a Wireless Paper came back as half a document under a
// 200, and the dashboard's parse error is the only symptom.
static bool serializedWhole(const JsonDocument& doc, String& out, bool pretty) {
  const size_t need = pretty ? measureJsonPretty(doc) : measureJson(doc);
  if (pretty) serializeJsonPretty(doc, out);
  else        serializeJson(doc, out);
  return !doc.overflowed() && out.length() == need;
}

// What a node says when it could not afford to answer. One string, because a
// caller telling the two apart by text has to match it exactly.
static const char* const kLowMemoryMsg =
  "ran out of memory while replying — nothing was changed; try again";

// The same answer for the one path that cannot afford to build it: sendJson()
// is what a handler fails *inside*, so it cannot say whether the work was
// done and must not claim it was not. Named once so its two uses cannot drift.
static const char* const kLowMemoryJson = "{\"error\":\"low memory\"}";

// A short literal reply that a browser must not keep. The refusals below are
// readings as much as the documents are, and the one it would hurt most to
// reuse: a browser holding on to a 503 goes on showing a node as out of memory
// long after it has recovered.
static void sendNoStore(AsyncWebServerRequest* r, int code, const char* type, const char* body) {
  AsyncWebServerResponse* res = r->beginResponse(code, type, body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// A reply that hands its body over rather than letting it be copied.
//
// request->send(code, type, String) builds an AsyncBasicResponse, and that
// constructor copies the body into a second String (WebResponses.cpp: the
// content length is then read off the copy). Both are alive at the same
// moment, so answering an eight-kilobyte /api/status wants two eight-kilobyte
// contiguous blocks, and a node whose heap has one left cannot find the
// second. What happens then is the worst of the outcomes available, because
// Arduino String fails an allocation by going empty rather than by saying so:
// the response takes its content length from the empty copy and the node
// answers 200 OK, Content-Length: 0. A caller sees a successful request with
// nothing in it, and no log line anywhere says why.
//
// Found on a node that had been up for days: /api/status and /api/qr both came
// back empty under a 200 while a two-byte reply from the same node was intact,
// and four other nodes answered those same requests with four to eight
// kilobytes. Note where that leaves the check above — it had done its job, the
// document serialised whole, and the reply was dropped one layer underneath
// it. A guard on our own serialisation cannot see this.
//
// So the body is moved into a filler the library calls as the socket drains,
// and it is never copied again. It sits behind a shared_ptr rather than being
// captured by move because the library copies the std::function twice on the
// way in — beginResponse takes it by value, and AsyncCallbackResponse's
// constructor assigns it again — and a String captured by move would be copied
// by both, which is the allocation this exists to remove. Copying a pointer
// costs nothing, and the body outlives the handler either way: the filler runs
// on the TCP task long after this function has returned.
//
// Returns nullptr when even that could not be afforded, which is a caller's
// cue to answer with something small. An empty body is one of those cases
// rather than a reply: no caller here can produce one honestly — sendJson()
// and handleExport() have already checked the document serialised to its
// measured length, handleBoardGet() starts from "[]", and a QR code always
// carries its own header — so a body that arrives empty is a String that
// failed to allocate and said nothing, which answered as 200 is exactly the
// silent empty reply this function exists to stop.
static AsyncWebServerResponse* ownedBodyResponse(AsyncWebServerRequest* r, int code,
                                                 const char* type, String&& body) {
  const size_t len = body.length();
  if (!len) return nullptr;
  AsyncWebServerResponse* res = nullptr;
  try {
    std::shared_ptr<String> held = std::make_shared<String>(std::move(body));
    res = r->beginResponse(type, len,
                           [held](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
                             const size_t have = held->length();
                             const size_t left = index < have ? have - index : 0;
                             const size_t n = left < maxLen ? left : maxLen;
                             if (n) memcpy(buf, held->c_str() + index, n);
                             return n;
                           });
  } catch (const std::bad_alloc&) {
    return nullptr;   // the pointer and its control block, not the body
  }
  res->setCode(code);                 // beginResponse throws rather than returning null
  return res;
}

static void sendJson(AsyncWebServerRequest* r, int code, const JsonDocument& doc) {
  String out;
  if (!serializedWhole(doc, out, false)) {
    sendNoStore(r, 503, "application/json", kLowMemoryJson);
    return;
  }
  AsyncWebServerResponse* res = ownedBodyResponse(r, code, "application/json", std::move(out));
  if (!res) {
    sendNoStore(r, 503, "application/json", kLowMemoryJson);
    return;
  }
  // Readings, not documents. A browser that reuses one of these shows an
  // uptime, a heap figure or a task phase from some earlier minute and gives
  // no sign it has done so — which is worse than a stale page, because a page
  // that looks wrong is noticed and a number that looks plausible is not.
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

static void sendError(AsyncWebServerRequest* r, int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(r, code, d);
}

// One JSON integer into a narrower settings field, through the shared width
// rule (SettingsRules::narrowInt — the comment there says why the JSON
// layer's own narrowing cannot be trusted with it). Absent or non-integer
// values leave the field alone, exactly as the is<int>() gates always have:
// a key the request does not carry is not part of the request. The value is
// read wide first — long long holds every integer JSON can carry — so an
// integer the field cannot hold is refused with a 400 rather than arriving
// at the validate*() rules as some other number. Sends the refusal itself;
// callers just return, as they do after authed().
template <typename T>
static bool jsonNarrow(AsyncWebServerRequest* r, JsonVariantConst v, T& out, const char* what) {
  if (!v.is<long long>()) return true;
  char msg[64];
  if (SettingsRules::narrowInt(v.as<long long>(), out, what, msg, sizeof(msg))) return true;
  sendError(r, 400, msg);
  return false;
}

// ---------------------------------------------------------------------------
bool WifiManager::wifiEnabled() const { return settings.links().wifiEnabled(); }

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
  { "console_tcp",         &MaintenanceSettings::consoleTcp },
  { "web_ui",              &MaintenanceSettings::webUi },
  { "mdns",                &MaintenanceSettings::mdns },
  { "rns_admin",           &MaintenanceSettings::rnsAdmin },
  { "lxmf_commands",       &MaintenanceSettings::lxmfCommands },
};

void WifiManager::begin() {
  // WiFi.setSleep() only reaches esp_wifi_set_ps() once the interface it
  // names has actually started; called any earlier (Power::begin() runs
  // before this) it just caches the request. STA_START/AP_START fire every
  // time an interface comes up — first boot, a mode change, a reconnect — so
  // hooking them here is what makes the profile's setting actually stick
  // rather than leaving the driver on whatever ESP-IDF defaulted to.
  WiFi.onEvent([](WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_START || event == ARDUINO_EVENT_WIFI_AP_START) {
      Power::applyWifiSleep();
      // Same lifecycle as the sleep setting: the TX ceiling only lands once
      // the driver is started, and starting is exactly what these events say.
      Power::applyWifiTxPower();
    }
    // Belt-and-braces for the listen interval: staConnect() writes it into
    // the config before the connect goes out, and this re-applies it after
    // every association in case something rebuilt the config in between.
    // Not STA_START — that fires inside WiFi.begin(), where a write from
    // this (event-task) hook races the config the core is building on the
    // caller's task. On a config that already holds the value the helper's
    // early-out makes this a read and no write, so an ordinary connect
    // never touches an associated station's config.
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) wifiManager.applyStaListenInterval();
  });
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
  // Split in two on purpose (Diag.h). The radio is what the switch is
  // supposed to buy, and the server below is what a node pays whether the
  // switch is on or off — so a node with Wi-Fi off shows exactly what making
  // the server lazy would be worth, on the board it is running on.
  Diag::cost(wifiEnabled() ? "wifi radio" : "netif stack (wifi off)");

  // The portal is the largest thing a small board can decline: 28624 B of
  // byte-addressable RAM with Wi-Fi on, measured on a Heltec Wireless Stick
  // that has about 213 KB of it and needs 53 KB for Reticulum. Off means the
  // routes are never registered and nothing listens on port 80; an operator
  // reaches the node over the console's own listener instead, which costs a
  // few hundred bytes to do the same job (ConsoleServer.h).
  const bool webUi = settings.maintenance().webUi;
  // Named once. Both boot paths — Wi-Fi off and Wi-Fi on — bill the same
  // thing, and two copies of the words meant a later change to one of them
  // would have made the bill disagree with itself depending on the path.
  const char* const kHttpDnsLabel = webUi ? "http + dns" : "dns (no portal)";
  // The resolver is not the portal's, and does not go off with it. Where the
  // USB link exists it runs whether or not Wi-Fi does: the link's lease names
  // the node as DNS and ESP-IDF's server cannot be told to name nobody
  // (UsbNcm.cpp), so with nothing on port 53 an attached host waits out its
  // full resolver timeout on every lookup instead of being refused and
  // moving on — which is the failure CaptiveDns exists to prevent. A board
  // with neither link has nothing to answer, and AsyncUDP's task goes unmade.
  //
  // The access point, not Wi-Fi in general. Those were the same question until
  // the two links were switched apart, and they are not the same now: a
  // station-only node has no AP_IP, so the address handed here is one it does
  // not hold, every query is refused, and the responder answers for nothing.
  // The USB link still wants it for the fast refusal described above.
  if (settings.links().wifiApEnabled || HAS_USB_NCM) {
    if (!_dns.begin(AP_IP)) log_w("captive DNS: could not bind port 53");
  }
  if (!webUi) {
    log_w("the web portal is switched off; HTTP :%d does not answer. The console does, "
          "on the cable and on TCP :%d", HTTP_PORT, CONSOLE_TCP_PORT);
  } else {
    setupRoutes();
    _http.begin();
  }

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
    Diag::cost(kHttpDnsLabel);
    if (webUi) log_i("HTTP :%d and RNS TCP :%d listening on every local link; Wi-Fi off",
                     HTTP_PORT, RNS_TCP_PORT);
    else       log_i("RNS TCP :%d listening on every local link; Wi-Fi and the portal are off, "
                     "and the console answers on TCP :%d", RNS_TCP_PORT, CONSOLE_TCP_PORT);
    return;
  }
  // Billed apart from the resolver above it. The two used to share a line, so
  // the one number covered a service the node needs and a convenience it does
  // not — which is no use at all when the question is what a board with eight
  // kilobytes left can afford to decline.
  Diag::cost(kHttpDnsLabel);
  startMdns();

  Diag::cost(settings.maintenance().mdns ? "mdns" : "mdns (off)");
  // What actually came up, which is no longer always an access point. Announcing
  // a SoftAP and its address on a station-only node is a log line that sends
  // somebody looking for a network that was never started — and the address it
  // printed, 0.0.0.0, looks like a fault rather than an absence.
  if (settings.links().wifiApEnabled) {
    log_i("SoftAP \"%s\" (%s) up at %s (http:%d, rns:%d)", _ssid, _securityName,
          WiFi.softAPIP().toString().c_str(), HTTP_PORT, RNS_TCP_PORT);
  } else {
    log_i("no access point: station only (http:%d, rns:%d on whatever the LAN gives)",
          HTTP_PORT, RNS_TCP_PORT);
  }
}

// The mDNS responder and its records, once per boot. Out of begin() because
// begin() is no longer the only Wi-Fi bring-up: a node that boots with Wi-Fi
// off returns before this and starts its network at runtime instead
// (syncRadioShape), and the responder has to come up on that path too.
// The boot-time Diag::cost lines stay in begin() — they are boot accounting.
// Once running, the component follows the AP netif's own down/up cycles by
// itself (apServicesDown says how); this is only ever needed once.
void WifiManager::startMdns() {
  _mdnsUp = true;   // the question is settled for this boot either way:
                    // maintenance.mdns is restart-applied, and a failed start
                    // already said so in the log
  if (!settings.maintenance().mdns) {
    log_i("mDNS: off — this node answers on its address, not by name "
          "(6 KB of byte-addressable RAM a small board can spend elsewhere)");
    return;
  }
  const bool webUi = settings.maintenance().webUi;
  if (MDNS.begin(_hostname)) {
    // Only the services that actually answer: a browser sent to _http._tcp on
    // a node whose portal is off gets a connection refused and no
    // explanation. Named once, and walked once, rather than the rule being
    // spelled again inside the loop.
    const char* services[2] = { "rns", nullptr };
    size_t nServices = 1;
    MDNS.addService("rns", "tcp", RNS_TCP_PORT);
    if (webUi) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      services[nServices++] = "http";
    }
    // So a browser or a script can tell the nodes apart without opening each
    // one — and, now, reach the one it picked: this is the delivery address,
    // the same value /api/status reports as lxmf_address. It used to be the
    // retimesh.node hash, which distinguished nodes perfectly well and was
    // useless for anything after that, since nothing announces it.
    //
    // Derived from the identity rather than read from the transport, which has
    // not started when mDNS is registered and would publish an empty string
    // here for the life of the record.
    for (size_t i = 0; i < nServices; i++) {
      const char* svc = services[i];
      MDNS.addServiceTxt(svc, "tcp", "name",  _ssid);
      MDNS.addServiceTxt(svc, "tcp", "node",  nodeIdentity.lxmfHex());
      MDNS.addServiceTxt(svc, "tcp", "fw",    FW_VERSION);
      MDNS.addServiceTxt(svc, "tcp", "board", BOARD_NAME);
    }
    if (webUi) log_i("mDNS: http://%s.local (rns on :%d) — browse _rns._tcp to find every node",
                     _hostname, RNS_TCP_PORT);
    else       log_i("mDNS: %s.local, rns on :%d — no portal is advertised, it is switched off",
                     _hostname, RNS_TCP_PORT);
  } else {
    log_w("mDNS start failed");
  }
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

// The radio's mode follows the two switches rather than the one. A station
// without an access point is now a shape this can be in — it is the shape a
// carried node wants, since the access point must beacon and cannot sleep —
// and asking for WIFI_AP_STA there would put the beacon back on the air.
//
// The idle policy's verdict is a term inside this rule, not a second rule
// beside it: while the policy holds the AP down (tick() mirrors its verdict
// into _apSuppressed), the settings shape simply has no AP in it, and every
// consumer — the convergence, the bring-up, the join-failure fallback —
// inherits that from the one place. Suppressed with no station to keep is
// the one shape the old rule never produced: the radio fully off.
wifi_mode_t WifiManager::settingsWifiMode(bool& wantAp, bool& wantSta) const {
  wantAp  = settings.links().wifiApEnabled && !_apSuppressed;
  wantSta = settings.links().wifiStaEnabled && stationConfigured();
  if (wantAp) return wantSta ? WIFI_AP_STA : WIFI_AP;
  if (!_apSuppressed || wantSta) return WIFI_STA;   // the shape this always was
  return WIFI_MODE_NULL;                            // idle-off, nothing else to run
}

// The station's listen interval — how many AP beacon intervals it may doze
// between wakes under the battery profile's max modem sleep (the driver
// ignores it in every other power-save mode). One read-modify-write of the
// station config: staConnect() calls it with the config freshly written and
// nothing in flight, the STA_CONNECTED hook re-applies it after every
// association, and the settings commit calls it for a live change. The
// driver reads a stored 0 as its default of 3, so 0 and 3 are one value
// with two spellings — normalized before the early-out, or a node on the
// default setting would rewrite an associated station's config on every
// unrelated live commit (and esp_wifi_set_config on an associated station
// may bounce the link). Whether the driver's doze then actually follows the
// interval on the air is a bench question; the write itself is now checked
// and logged.
void WifiManager::applyStaListenInterval() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;   // no station interface up
  const uint8_t want = settings.wifi().staListenInterval;
  const uint8_t have = conf.sta.listen_interval ? conf.sta.listen_interval : 3;
  if (have == want) return;                                        // nothing to write
  conf.sta.listen_interval = want;
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
  if (err != ESP_OK)
    log_w("station: listen interval %u not applied (err 0x%x) — the driver dozes at its default",
          (unsigned)want, err);
  else
    // The bench watches for this: a rewrite from the STA_CONNECTED hook
    // means something rebuilt the config after staConnect()'s clean write.
    log_i("station: listen interval %u -> %u written", (unsigned)have, (unsigned)want);
}

// One station connect for every site that starts a join — the boot's, the
// glass's, and the fallback after a failed join. WiFi.begin() alone rebuilds
// the whole station config (a memset, core STA.cpp) and issues the connect
// in the same call, and esp_wifi_set_config is documented to refuse with
// ESP_ERR_WIFI_STATE while that connect is in flight (esp_wifi.h) — so a
// listen interval applied "right after begin()" was a write into a closing
// door, and the discarded refusal left the driver on its default. The core
// offers the two halves separately: begin() with connect=false does
// everything up to the connect — interface up, config written, dynamic IP
// asked for — which opens a quiet gap for the read-modify-write, and
// esp_wifi_connect() is then exactly the call the core's own tryConnect
// path would have made. The core's auto-reconnect and WiFi.reconnect()
// re-use the stored config without rebuilding it (STA.cpp), so what is
// written here is what every later association runs.
//
// The halves are called on WiFi.STA directly because WiFi.begin()'s
// wl_status_t cannot gate this: on success it returns the sticky
// STA.status(), which the event handler parks at WL_CONNECT_FAILED after
// any AUTH_FAIL and which begin/connect/disconnect never reset (core
// WiFiSTA.cpp, STA.cpp) — so after one wrong-password association, every
// later configure of the station, including one carrying the corrected
// password, would read as a failure and never reach the connect. The
// booleans are the real results of the same two calls WiFi.begin() makes.
void WifiManager::staConnect(const char* ssid, const char* password) {
  if (!WiFi.STA.begin(/*tryConnect=*/false) ||
      !WiFi.STA.connect(ssid, (password && password[0]) ? password : nullptr,
                        0, nullptr, /*tryConnect=*/false)) {
    log_w("station: could not configure \"%s\"", ssid);
    return;                              // the core did not take the config; nothing to connect
  }
  applyStaListenInterval();              // clean write: the connect has not been issued yet
  const esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) log_w("station: connect to \"%s\" failed (err 0x%x)", ssid, err);
}

// WPA needs 8..63 characters; anything else means an open network. One rule
// for the password softAP() runs with and the auth mode the config patch
// writes, so the two cannot drift apart.
static bool apSecured(const WifiSettings& w) {
  return w.security != ApSecurity::Open && strlen(w.password) >= 8;
}

// One read-modify-write of the AP config. After softAP(): the Arduino core
// builds its config with beacon_interval = 100 on every softAP() call and
// writes it whenever any field it compares differs (core AP.cpp —
// softap_config_equal ignores beacon_interval, so an identical call skips
// the write and a standing patch survives it, but any real change writes the
// fresh config and silently reverts the beacon). The beacon slows on every
// shape of AP (400 TU ≈ 410 ms: a 4x cut in beacon airtime and current, for
// phones taking a moment longer to list the network — Config.h says why this
// is a build default rather than a setting); the auth mode joins the same
// transaction only where WPA3 is asked for, because the Arduino wrapper
// knows only open/WPA2 — through ESP-IDF the AP config accepts
// WPA2_WPA3_PSK / WPA3_PSK (cipher forced to CCMP, PMF implied by SAE), and
// mixed mode lets WPA2-only clients still join.
//
// esp_wifi_set_config is documented to refuse with ESP_ERR_WIFI_STATE while
// a station connect is in flight (esp_wifi.h). At boot the caller dodges
// that by ordering this before the join; at a runtime AP re-up the station
// is not ours to schedule around — the 30 s watchdog's reconnect keeps a
// connect in flight much of the time on a LAN-down node — so that refusal
// is reported through inFlight and tick() retries just this RMW until the
// connect settles. Every other failure is logged here and final.
//
// The auth-mode inputs come from the snapshot startAccessPoint() froze when
// it armed the patch, never from settings.wifi() here: a save with restart-
// applied fields can land between two retries, and a retry that re-read the
// settings would patch the next boot's security onto the AP the operator
// was just told keeps its shape until the reboot (the members say more).
bool WifiManager::applyApConfigPatch(bool& inFlight) {
  inFlight = false;
  const bool patchWpa3 = _apPatchWpa3;
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_AP, &conf) != ESP_OK) {
    log_w("AP config could not be read — the beacon%s patch was skipped",
          patchWpa3 ? "/WPA3" : "");
    return false;
  }
  conf.ap.beacon_interval = WIFI_AP_BEACON_TU;
  if (patchWpa3) {
    conf.ap.authmode = _apPatchWpa3Only ? WIFI_AUTH_WPA3_PSK
                                        : WIFI_AUTH_WPA2_WPA3_PSK;
    conf.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
  }
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &conf);
  if (err == ESP_OK) {
    if (patchWpa3)
      _securityName = _apPatchWpa3Only ? "wpa3" : "wpa2wpa3";
    return true;
  }
  inFlight = (err == ESP_ERR_WIFI_STATE);
  if (!inFlight)
    log_w("AP config patch rejected by the Wi-Fi driver (err 0x%x) — beacons stay at "
          "100 TU%s", err, patchWpa3 ? " and the AP stays on WPA2" : "");
  return false;
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

  const bool secured = apSecured(w);         // the one WPA-validity rule, above
  const char* pass = secured ? w.password : nullptr;

  bool wantAp, wantSta;
  const wifi_mode_t mode = settingsWifiMode(wantAp, wantSta);

  WiFi.persistent(false);
  WiFi.mode(mode);
  // IPv6 link-local on both links, asked for before they start: core 3
  // creates the address when the interface comes up and only then, so a
  // request made afterwards — which is when AutoInterface used to make it —
  // waits for a start that has already happened. AutoInterface reads the
  // addresses; the links are brought up here, so they are enabled here. And
  // only for it: AutoInterface is this firmware's one consumer of link-local
  // IPv6, so with peering off the links stay IPv4-only rather than paying
  // DAD, MLD reports and router solicitations on both for nothing.
  const bool wantIPv6 = AutoInterface::wanted();
  if (wantAp) {
    if (wantIPv6) WiFi.softAPenableIPv6();
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(_ssid, pass, w.channel, w.hidden ? 1 : 0, w.maxStations);
  }
  if (wantIPv6) WiFi.enableIPv6();

  _securityName = secured ? "wpa2" : "open";

  const bool wantWpa3 = secured && w.security != ApSecurity::WPA2;
  if (wantWpa3 && !WPA3_SOFTAP_SUPPORTED)
    log_w("WPA3 needs an ESP-IDF 5 core; this build runs the AP as WPA2");
  // The AP config read-modify-write (applyApConfigPatch above), and it must
  // sit exactly here: after softAP() built the config, and before the
  // station join below — esp_wifi_set_config refuses with ESP_ERR_WIFI_STATE
  // while a station connect is in flight, so sitting after the join exposed
  // the patch to that refusal on every AP+STA boot. This ordering only
  // protects the boot path, though: at a runtime AP re-up the watchdog's
  // reconnect can already have a connect in flight, so that one refusal
  // arms a bounded retry that tick() serves once the connect settles.
  if (wantAp) {
    // The patch inputs are frozen here, at arm time, from the same settings
    // read this bring-up runs on. Every attempt — the one below and tick()'s
    // retries — patches THIS shape; a save landing mid-retry changes the
    // next boot, never a standing retry (WifiManager.h, at the members).
    _apPatchWpa3     = wantWpa3 && WPA3_SOFTAP_SUPPORTED;
    _apPatchWpa3Only = (w.security == ApSecurity::WPA3);
    bool inFlight = false;
    if (applyApConfigPatch(inFlight)) {
      _apPatchRetries = 0;
    } else if (inFlight) {
      _apPatchRetries = kApPatchAttempts;
      _apPatchGate.prime(millis());
      log_w("AP config patch deferred: a station connect is in flight — retrying from tick() "
            "(beacons at 100 TU%s until it lands)",
            wantWpa3 && WPA3_SOFTAP_SUPPORTED ? ", WPA2" : "");
    } else {
      _apPatchRetries = 0;                 // a final rejection; the helper logged it
    }
  }

  // Station mode: join the configured LAN too. The AP and the STA share
  // one radio, so the AP follows the LAN's channel once connected. Not when
  // the station is already associated: this function is also the AP's
  // runtime re-up path (tick()'s convergence — one bring-up, so the beacon
  // patch, WPA3 and IPv6 reapply by construction), and on an AP+STA node a
  // wake would otherwise rebuild the config and re-issue the connect under
  // a healthy association, bouncing the LAN link to bring back the AP. At
  // boot the station cannot be connected yet, so the boot path is unchanged.
  if (wantSta && !stationConnected()) {
    WiFi.setAutoReconnect(true);
    staConnect(w.staSsid, w.staPassword);
    log_i("station: joining \"%s\"", w.staSsid);
    _staRetryAt = millis() + 30000;
  }
}

void WifiManager::tick() {
  // The AP idle policy, ahead of everything: neither block touches the
  // driver beyond a read, so they are safe wherever a pass is — including
  // the passes below that return early while a join is still trying, which
  // would otherwise sit on a wake for the join window's full twenty seconds.
  //
  // The wake first, so the pass that serves it evaluates nothing stale: the
  // policy's clock re-arms, the suppression term drops, and the convergence
  // below (raised here, served with nothing in flight) brings the AP back —
  // the whole return is one tick pass plus the driver's own start-up.
  if (_apWakeReq) {
    _apWakeReq = false;
    _apIdle.wake(millis());
    if (_apSuppressed) {
      _apSuppressed = false;
      _modeSyncReq = true;
      log_i("access point: woken — coming back up");
    }
  }
  // A Wi-Fi save with restart-applied fields landed (cancelApConfigPatch):
  // any patch retry still armed was built for the settings before that save,
  // and serving it now would read the new ones — patching the next boot's
  // security onto the running AP. Served here, ahead of the convergence, so
  // a bring-up later in this same pass may re-arm afresh; that retry is
  // current by construction.
  if (_apPatchCancelReq) {
    _apPatchCancelReq = false;
    _apPatchRetries = 0;
  }
  // The verdict, at a 1 s cadence — softAPgetStationNum() is a driver call,
  // and the policy needs no finer clock than the minutes it counts in. With
  // the feature off the policy answers "never" from its first line and the
  // station count is never asked for: a node with the switch off runs the
  // ask as two setting reads, the mode-bit read and a comparison. The body
  // lives in askApIdlePolicy() because the gate only bounds how often the
  // driver is polled, never the final word: syncRadioShape makes one more,
  // ungated ask at the instant a staged teardown comes due.
  if (_apIdleGate.due(millis())) askApIdlePolicy();
  // The glass's asks are served first, on the task that owns the driver. The
  // starts live here rather than where they were asked so that no other task
  // ever calls into the driver: a display-task scanNetworks()/begin() raced
  // the convergence below — tick() could observe nothing in flight, then
  // strip with WiFi.mode() the STA bit the display task was raising at that
  // very moment, aborting its scan in the driver. With the starts on this
  // task there is no second task left to race.
  if (_joinReq) {
    _joinReq = false;
    if (wifiEnabled()) {
      _joinDeadline = millis() + 20000;  // WPA against a present AP settles well inside this
      _joining = true;
      WiFi.disconnect();                 // whatever the station was doing before
      staConnect(_joinSsid, _joinPass);
      log_i("station: joining \"%s\" (asked from the glass)", _joinSsid);
    } else {
      // Wi-Fi went off between the ask and this pass. Starting the join
      // would start the very driver the switch keeps down, so the ask fails
      // instead; the glass reads the verdict off its usual shelf.
      _joinVerdict = 2;
    }
  }
  // The join completes here too, still on the one task — connect, timeout,
  // persist and fallback all in one place, so an abandoned screen can no
  // longer strand any of them.
  if (_joining) {
    if (WiFi.status() == WL_CONNECTED) {
      WifiSettings w = settings.wifi();
      strlcpy(w.staSsid, _joinSsid, sizeof(w.staSsid));
      strlcpy(w.staPassword, _joinPass, sizeof(w.staPassword));
      char why[96];
      // Proven on air, then written down — through the same rules the
      // funnel holds, so no side door into the store.
      if (SettingsRules::validateWifi(w, why, sizeof(why))) {
        settings.saveWifi(w);
        // The switch follows the proof. A join asked for at the glass on a
        // node whose station switch was off has just run a station anyway,
        // so the settings say so too — or the re-assert below would drop
        // the network the person just joined, and a reboot would keep the
        // credentials but never the link. Straight to saveLinks(), not
        // through applyLinks(): that funnel restarts the node for shapes
        // the running radio cannot take, and this is the one it already
        // holds.
        if (!settings.links().wifiStaEnabled) {
          LinkSettings l = settings.links();
          l.wifiStaEnabled = true;
          settings.saveLinks(l);
          log_i("station: the station switch was off — the join turned it on");
        }
        log_i("station: joined \"%s\", IP %s — saved", w.staSsid,
              WiFi.localIP().toString().c_str());
      } else {
        log_w("station: joined \"%s\" but not saved: %s", _joinSsid, why);
      }
      _staRetryAt = millis() + 30000;
      _joining = false;
      _joinVerdict = 1;
      _modeSyncReq = true;               // the verdict ends at the re-assert below
    } else if ((int32_t)(millis() - _joinDeadline) >= 0) {
      log_w("station: could not join \"%s\"", _joinSsid);
      const WifiSettings& w = settings.wifi();
      WiFi.disconnect();
      bool wantAp, wantSta;
      settingsWifiMode(wantAp, wantSta);
      if (wantSta) {
        // The settings ask for the stored station (switch on, network
        // stored) — fall back to it and let the watchdog take over. The
        // re-assert below is a no-op on this path: the shape it computes
        // keeps the STA bit this begin() needs.
        staConnect(w.staSsid, w.staPassword);
        _staRetryAt = millis() + 30000;
      }
      // Nothing to fall back to — none stored, or the switch off — is no
      // longer a branch of its own: the join's staConnect() was the only
      // thing keeping the station interface up, and the re-assert below
      // takes it down with everything else the settings do not ask for.
      _joining = false;
      _joinVerdict = 2;
      _modeSyncReq = true;
    } else {
      return;                            // still trying; the watchdog waits its turn
    }
  }
  // The scan the glass asked for starts here, with no join resolving — the
  // join block above returns while one is still trying, so a scan request
  // raised mid-join is simply parked (staScanCount() answers "still
  // scanning" for it) and served on the pass after the verdict.
  if (_scanReq) {
    if (wifiEnabled()) {
      _scanActive = true;                // claimed before the start, as before
      if (_scanReq) {                    // re-checked after the claim: a screen
        WiFi.scanNetworks(true /* async */);   // closed right now cancels an
        _scanReq = false;                //   unserved request (staScanDone())
      } else {
        _scanActive = false;             // cancelled between the check and the claim
      }
    } else {
      _scanReq = false;                  // racing Wi-Fi-off: see the join above
    }
  }
  // The one place the radio's shape is decided while the node runs. Both
  // verdicts above land here, and so do the glass's endings on the display
  // task (a scan's results freed, a network forgotten): whatever the STA bit
  // was raised for is over, so the radio goes back to the shape the settings
  // ask — computed after the saves above, which is what lets a join that has
  // just stored its network and its switch keep what it proved. Held while a
  // scan is out: the core raised the STA bit for it inside scanNetworks(),
  // and stripping the bit aborts the scan in the driver — staScanDone()
  // raises the request again, and it is served then. Held just the same
  // while an ask is pending but unserved: the requests above are served
  // earlier in this very pass, so a pending flag here means it arrived
  // moments ago from the display task, and it must reach its serve whole
  // rather than have the mode settled across it. The flag drops before the
  // settings are read, so a request racing in from the display task is kept
  // whole for the next pass rather than half-served by this one. The body
  // lives in syncRadioShape(): it is no longer a bare mode assertion, because
  // the shape can now change while the node runs — the idle policy and the
  // live links.wifi_ap switch take the AP down and up — and a transition
  // carries services with it and stages a grace before any teardown.
  // A task that stopped itself — rebuildDiscovery lost its socket and could
  // not rebind — has no convergence of its own to ride: nothing else raises
  // the sync when the radio's shape already matches the settings, so the
  // restart the flag exists for waited on some unrelated change (perhaps for
  // ever). Raised here instead — one relaxed atomic read on the common path
  // — and consumed by the need-based restart at the end of syncRadioShape.
  // Gated on a link actually running: with the radio down the restart must
  // wait for whatever convergence brings a link back anyway, and a raise
  // nothing can consume would re-run the convergence every pass for good.
  if (AutoInterface::stoppedUnexpectedly() && AutoInterface::wanted() &&
      WiFi.getMode() != WIFI_MODE_NULL)
    _modeSyncReq = true;
  if (_modeSyncReq && !_scanActive && !_scanReq && !_joinReq) {
    _modeSyncReq = false;
    syncRadioShape();
  }
  // The AP config patch a runtime re-up could not land: the driver refused
  // esp_wifi_set_config(AP) with ESP_ERR_WIFI_STATE because a station
  // connect was in flight (on a LAN-down node the 30 s watchdog keeps one
  // going much of the time). Re-run just the RMW — not the whole bring-up —
  // until it lands or the attempts run out.
  if (_apPatchRetries && _apPatchGate.due(millis())) {
    if (!apUp()) {
      _apPatchRetries = 0;                 // the AP left; its next bring-up patches afresh
    } else {
      bool inFlight = false;
      if (applyApConfigPatch(inFlight)) {
        _apPatchRetries = 0;
        log_i("AP config patch landed on retry: %s, %u TU beacons", _securityName,
              (unsigned)WIFI_AP_BEACON_TU);
      } else if (!inFlight) {
        _apPatchRetries = 0;               // a final rejection; the helper logged it
      } else if (--_apPatchRetries == 0) {
        log_w("AP config patch still refused after %u attempts — a station connect never "
              "settled; beacons stay at 100 TU until the next AP cycle",
              (unsigned)kApPatchAttempts);
      }
    }
  }
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
// The convergence body: the one place the running radio is taken toward the
// shape the settings and the idle policy ask for. On the loop task only.
//
// Transition rules, in the order they are applied:
//   - A teardown that removes the AP waits out AP_STOP_GRACE_MS first, so
//     the reply to whatever asked (an HTTP 200 riding the AP itself, a
//     console OK) leaves before its link does — the same reasoning as
//     Bootloader::reboot's folded delay. A wake or a switch flip during the
//     grace simply changes what the next pass computes, and the stage drops.
//     The grace also outlasts the idle gate's cadence (the invariant lives
//     with the two constants: Config.h and the gate in WifiManager.h), so a
//     station that associates during it is seen by at least one policy ask,
//     which lifts the suppression and drops the stage the same way.
//   - The AP coming up goes through startAccessPoint(), the one bring-up,
//     so the beacon read-modify-write, WPA3, and IPv6 reapply by
//     construction; the Wi-Fi sleep and TX-power settings reapply from the
//     AP_START/STA_START event hook begin() installs. Captive DNS and (on a
//     node that booted with Wi-Fi off) mDNS follow the AP's OBSERVED state,
//     reconciled once at the end of every pass (the body's last statement
//     says why observed, never intended).
//   - Dependent services ride the same pass as the driver change:
//     AutoInterface::end() before the netifs go (its sockets hold the
//     multicast group on them), begin() once a link is back. It keeps
//     running when only the AP drops but a station remains — a peer on the
//     LAN is exactly what it is for.
// ---------------------------------------------------------------------------
void WifiManager::syncRadioShape() {
  bool wantAp = false, wantSta = false;
  wifi_mode_t want = WIFI_MODE_NULL;   // Wi-Fi off in settings asserts nothing
  if (wifiEnabled()) want = settingsWifiMode(wantAp, wantSta);
  const wifi_mode_t have  = WiFi.getMode();   // WIFI_MODE_NULL while the driver is down
  const bool haveAp = (have & WIFI_MODE_AP) != 0;
  const bool dropAp = haveAp && !wantAp;
  if (!dropAp) _apDownStaged = false;         // a wake mid-grace cancels the stage
  if (want != WIFI_MODE_NULL) _autoIfEndFails = 0;   // the teardown these count was called off
  if (have != want) {
    if (dropAp) {
      if (!_apDownStaged) {
        _apDownStaged = true;
        _apDownDueMs  = millis() + AP_STOP_GRACE_MS;
      }
      if ((int32_t)(millis() - _apDownDueMs) < 0) {
        _modeSyncReq = true;                  // come back next pass; grace still running
        return;
      }
      _apDownStaged = false;
      // The teardown is due — one FINAL policy ask first, with inputs read
      // this instant. The gate's cadence bounds how often tick() polls the
      // station count, so the newest verdict can be almost a second old: a
      // station that associated after that ask, in the grace's last stretch,
      // would be cut off by a verdict that never saw it. The policy stays
      // the only rule — suppressed() lifts its own latch on a station while
      // the AP is still on the air — this only asks it once more, ungated.
      askApIdlePolicy();
      if (wifiEnabled()) {
        bool nowAp = false, nowSta = false;
        settingsWifiMode(nowAp, nowSta);
        if (nowAp) return;                    // the arrival lifted the latch; the ask
      }                                       //   raised the sync, and the next pass
    }                                         //   converges on the AP it kept
    if (want == WIFI_MODE_NULL) {
      // Nothing left to run. AutoInterface goes first — end() exists so the
      // netifs are never cycled under its joined discovery group — and the
      // driver stops with the mode. (Closing its sockets is also what drops
      // their multicast memberships, so this path never strands one.)
      //
      // And it now confirms: false means the task never answered inside
      // end()'s 5 s, and stopping the driver under its joined group is the
      // stranded-membership failure this ordering exists to prevent — so
      // the mode change is NOT taken this pass and the teardown retries
      // (re-walking the grace above, which keeps the staging rule in one
      // place at the cost of a second short grace nobody sees). Bounded by
      // kAutoIfEndFailsMax — the trade is written at its definition.
      if (!AutoInterface::end()) {
        if (++_autoIfEndFails < kAutoIfEndFailsMax) {
          _modeSyncReq = true;
          return;
        }
        log_e("AutoInterface: never confirmed its stop after %u passes — taking the radio "
              "down under it rather than holding it on for ever",
              (unsigned)kAutoIfEndFailsMax);
      }
      _autoIfEndFails = 0;
      WiFi.mode(WIFI_MODE_NULL);
      log_i("wifi: radio off%s", _apSuppressed ? " (access point idled down; a wake brings it back)" : "");
    } else {
      // A netif being stripped while AutoInterface keeps running must leave
      // the discovery group first, while that netif is still alive: asked
      // after the mode change, the leave hits lwIP's netif-gone ENXIO path,
      // which returns before the socket's membership slot (one of
      // CONFIG_LWIP_MAX_SOCKETS = 16) is unregistered — each AP idle-down
      // cycle then stranded a slot until every join was refused, for good.
      if (dropAp) AutoInterface::linkDown("WIFI_AP_DEF");
      if ((have & WIFI_MODE_STA) && !(want & WIFI_MODE_STA))
        AutoInterface::linkDown("WIFI_STA_DEF");
      if (wantAp && !haveAp) {
        startAccessPoint();                   // services follow at the reconcile below
        log_i("access point \"%s\" (%s) back up at %s", _ssid, _securityName,
              WiFi.softAPIP().toString().c_str());
      } else {
        WiFi.mode(want);
        if (dropAp) log_i("access point down; the radio stays up for the station");
      }
    }
  }
  // Need-based, not stop-keyed: whenever the radio runs a link, the settings
  // want peering, and the task is not up, it is started — whoever stopped it
  // and whether it ever ran. The old condition keyed on who had stopped it
  // (this path's own end(), or the task's self-stop after losing its
  // discovery socket), and a node that booted with both Wi-Fi switches off
  // had neither: the first live AP switch-on brought the radio up and
  // peering never followed until a reboot. No retry storm hides in the
  // broader condition — this runs only on convergence passes, and begin()
  // is idempotent while a task lives (its sTaskAlive guard), so a pass that
  // finds one up costs a couple of flag reads.
  if (want != WIFI_MODE_NULL && AutoInterface::wanted() && !AutoInterface::enabled()) {
    if (AutoInterface::stoppedUnexpectedly())
      log_i("AutoInterface: restarting — the task had stopped itself after losing its discovery socket");
    AutoInterface::begin();
  }
  // The AP's services follow its OBSERVED state — the mode bit the radio
  // actually runs — reconciled once here, at the end of every pass that runs
  // to completion. They used to move on intent instead: down at the moment a
  // teardown committed, up only inside the wantAp && !haveAp bring-up branch.
  // The end()-retry leg above returns between those two, and when a station
  // arrival or a wake then kept the AP, no later pass changed the mode — so
  // the AP beaconed on with port 53 closed and nothing in the log until the
  // next real AP cycle. Keyed on the observation, a kept AP gets its resolver
  // back on the very next pass, whatever kept it. Both calls are idempotent
  // (_dns.end() on a closed socket is nothing; up re-binds only behind
  // !_dns.listening() and !_mdnsUp), so the steady state costs a mode read
  // and a flag check. And no down-before-teardown ordering is lost by having
  // no early down: unlike AutoInterface's discovery sockets, the resolver
  // binds the ANY address and holds no membership on the AP netif
  // (CaptiveDns::begin), so a netif cycling under it neither strands nor
  // breaks anything — the resolver simply follows the radio down here, a few
  // statements after the mode lands, and stays answering for the clients of
  // an AP that is still on the air. The early returns above skip this, and
  // may: each of them leaves mode and services both untouched, and each
  // guarantees a follow-up pass (_modeSyncReq, or the ask's own raise), so
  // services match apUp() at every pass exit.
  if (apUp()) apServicesUp();
  else        apServicesDown();
}

// The idle policy asked with inputs read this instant, its verdict mirrored
// into the suppression term the settings shape reads, and the convergence
// raised on any change. The one place the policy is fed: tick() calls this
// behind the 1 s SampleGate (softAPgetStationNum is a driver call, and the
// policy counts minutes), and syncRadioShape() calls it once more, ungated,
// at the moment a staged AP teardown comes due — the gate bounds the polling
// cadence, never the final word.
void WifiManager::askApIdlePolicy() {
  const bool en = settings.wifi().apIdleOff && settings.links().wifiApEnabled;
  const bool up = apUp();
  const bool suppress = _apIdle.suppressed(
      millis(), en, up, (en && up) ? WiFi.softAPgetStationNum() : 0,
      (uint32_t)settings.wifi().apIdleMinutes * 60000u);
  if (suppress != _apSuppressed) {
    _apSuppressed = suppress;
    _modeSyncReq = true;               // the transition rides the one convergence
    if (suppress)
      log_i("access point: empty for %u min — going down (the button, SET links.wifi_ap on "
            "or WIFI ON at the console, or an admin message brings it back)",
            (unsigned)settings.wifi().apIdleMinutes);
    else
      log_i("access point: idle-off lifted — coming back up");
  }
}

// What leaves the air with the access point, and what returns with it. mDNS
// is deliberately absent from the down path: the pinned espressif/mdns
// component (^1.9.0 via the framework's dependencies.lock) registers its own
// WIFI_EVENT handlers at mdns_init() — the framework sdkconfig compiles the
// predefined AP/STA netifs in (CONFIG_MDNS_PREDEF_NETIF_AP=y) — so
// WIFI_EVENT_AP_STOP disables the AP's PCBs and WIFI_EVENT_AP_START
// re-enables them, and enabling probes and re-announces the hostname and
// services on that netif (mdns.h documents enable as "probe, resolve
// conflicts and announce"). The station side is untouched either way, which
// is exactly the behaviour wanted; restarting the responder here would
// disturb it for nothing.
void WifiManager::apServicesDown() {
  // The resolver stays where the USB link exists: its lease names the node
  // as DNS whether or not Wi-Fi runs, and an unanswerable port 53 makes an
  // attached host wait out its resolver timeout on every lookup (begin()
  // tells the same story at boot).
  if (!HAS_USB_NCM) _dns.end();
}

void WifiManager::apServicesUp() {
  // The same condition begin() binds under, re-checked because the boot may
  // have skipped it: a node that started with Wi-Fi fully off never bound.
  if ((settings.links().wifiApEnabled || HAS_USB_NCM) && !_dns.listening()) {
    if (!_dns.begin(AP_IP)) log_w("captive DNS: could not bind port 53");
  }
  // A node that booted with Wi-Fi off returned from begin() before its mDNS
  // block; the first runtime up is where the responder can finally start.
  // Once started it manages the AP netif's cycles itself (above).
  if (!_mdnsUp) startMdns();
}

const char* WifiManager::apStateName() const {
  if (!settings.links().wifiApEnabled) return "off";
  if (_apSuppressed) return "idle-off";
  return apUp() ? "up" : "down";
}

// ---------------------------------------------------------------------------
// Joining a network from the glass (WifiManager.h explains the live-first
// order). All of it is polled from the display task; the driver does the
// actual work on its own task either way.
// ---------------------------------------------------------------------------
void WifiManager::staScanStart() {
  // A pure request: tick() calls scanNetworks() on the loop task, so the
  // scan starts on whatever shape the convergence there has settled rather
  // than racing it. (Scanning wants the station interface, and the core
  // provides it: scanNetworks() calls WiFi.enableSTA(true) itself
  // (WiFiScan.cpp), which adds the STA bit to whatever shape is running —
  // a station-only node's scan is mode-neutral, and an AP-only node gains
  // STA for the scan's duration only.) The one-pass start latency never
  // shows on the glass: staScanCount() answers "still scanning" for a
  // request not yet served, and the screen polls at 250 ms anyway.
  _scanReq = true;
}

int WifiManager::staScanCount() {
  // Asked for but not started yet: before any scan the core's scanComplete()
  // answers WIFI_SCAN_FAILED, which maps to 0 below — and an unserved
  // request read as "finished, nothing found" would make the glass free a
  // table it never had and cancel the scan before tick() could start it.
  if (_scanReq) return -1;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n == WIFI_SCAN_FAILED)  return 0;
  return n;
}

bool WifiManager::staScanResult(int i, StaScanEntry& out) {
  if (i < 0 || i >= WiFi.scanComplete()) return false;
  strlcpy(out.ssid, WiFi.SSID(i).c_str(), sizeof(out.ssid));
  out.rssi    = (int8_t)WiFi.RSSI(i);
  out.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  return out.ssid[0] != '\0';           // hidden networks scan as empty names
}

void WifiManager::staScanDone() {
  // A request tick() has not served yet is cancelled first — a screen closed
  // in the ask-to-serve window would otherwise have tick() start a scan
  // nobody polls, holding the STA promotion (and the convergence gate) for
  // ever. tick() re-checks the flag after claiming _scanActive, so the
  // cancel lands on either side of the claim; in the residual few-
  // instruction window where the start still goes out, _scanActive ends up
  // false and _modeSyncReq true, and the convergence pass strips the
  // promoted bit from under the orphan scan — an abort, not a strand.
  _scanReq = false;
  // Only the result table is given back here; the mode is not touched on
  // this task. scanDelete() also clears the core's scanning/done bits, so a
  // scan abandoned mid-flight cannot wedge the next one. The request hands
  // the unwind to tick(), which re-asserts the settings shape at its next
  // pass with no scan or join in flight — a join mid-attempt simply keeps
  // the request parked until its verdict.
  WiFi.scanDelete();
  _scanActive = false;
  _modeSyncReq = true;
}

bool WifiManager::staJoin(const char* ssid, const char* password) {
  // What cannot be stored is not attempted: a 64-character raw PSK joins
  // fine and then wedges every wifi.* change against the funnel's length
  // rule — the funnel's truth holds at this door too.
  if (!ssid || !ssid[0] || strlen(ssid) > 32) return false;
  if (password && strlen(password) > 63) return false;
  strlcpy(_joinSsid, ssid, sizeof(_joinSsid));
  strlcpy(_joinPass, password ? password : "", sizeof(_joinPass));
  // State before flag, like every hand-off here: the credentials land, then
  // the request. No driver call is made on this task at all — tick() serves
  // the ask on the loop task (disconnect, begin(), deadline), and its
  // verdict settles the final shape by re-asserting what the settings ask
  // for, which a success has just updated to keep the station.
  // staJoinState() reports Trying off the flag alone, so the screen shows
  // "Joining..." without waiting a pass for the serve.
  _joinReq = true;
  return true;
}

WifiManager::StaJoin WifiManager::staJoinState() {
  // Pure, and one-shot on the verdicts: the work happens in tick() on the
  // loop task. The screen's own poll once carried it, and a screen closed
  // mid-join stranded the station, the watchdog, and the typed password.
  // An ask tick() has not served yet is already Trying — the glass's
  // "Joining..." must not flicker through Idle while the request waits.
  if (_joinReq || _joining) return StaJoin::Trying;
  const uint8_t v = _joinVerdict;
  if (v) {
    _joinVerdict = 0;
    return v == 1 ? StaJoin::Joined : StaJoin::Failed;
  }
  return StaJoin::Idle;
}

void WifiManager::staForget() {
  _joinReq = false;                      // a forget mid-attempt wins — asked or
  _joining = false;                      //   already trying alike
  WifiSettings w = settings.wifi();
  if (!w.staSsid[0]) return;
  log_i("station: forgetting \"%s\"", w.staSsid);
  w.staSsid[0] = '\0';
  w.staPassword[0] = '\0';
  settings.saveWifi(w);
  WiFi.disconnect();                     // stationConfigured() is now false; the watchdog rests
  // The disconnect drops the link, not the interface: the STA the boot or a
  // join brought up stays started, so an AP + stored-station node would keep
  // WIFI_AP_STA until reboot after an ordinary forget. The re-assert of the
  // settings shape is tick()'s — this task decides no modes — and the save
  // above landed before the flag, so the convergence it triggers reads the
  // forgetting, never the forgotten.
  _modeSyncReq = true;
}

int WifiManager::staRssi() const { return stationConnected() ? WiFi.RSSI() : 0; }

void WifiManager::staIpText(char* out, size_t n) const {
  // Octets, not String: this runs at 1 Hz on the display task while the
  // wifi page is open, and the UI path is otherwise allocation-free.
  const IPAddress ip = WiFi.localIP();
  snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// ---------------------------------------------------------------------------
// HTTP Basic Auth against the admin password. Sends the 401 challenge
// itself when it fails, so callers just `return`.
// ---------------------------------------------------------------------------
// A page from the node's filesystem, with the one header that stops a browser
// showing an old one for ever.
//
// Nothing here sent Cache-Control, an ETag or Last-Modified, so a browser had
// no validator and no instruction and fell back to heuristic caching: it kept
// whatever it saw first and did not ask again. A node that has just been
// reflashed then goes on serving a page nobody sees, and the asset stamp
// cannot tell anyone, because it compares the firmware against the *node's*
// filesystem and knows nothing about a copy held in somebody's browser.
//
// This is worth fixing on its own account, and it is not what broke the
// settings page — that was a syntax error in the page itself, and this header
// was added while chasing the wrong explanation for it. Both are here because
// both are real; only one of them was the bug.
//
// "no-cache" rather than "no-store": the browser may keep the copy, it just
// has to ask before using it. These are tens of kilobytes over a local link,
// and being right matters more than saving the round trip.
static const char kNoCache[] = "no-cache";

static void sendPage(AsyncWebServerRequest* request, const char* path) {
  AsyncWebServerResponse* res = request->beginResponse(LittleFS, path, "text/html");
  res->addHeader("Cache-Control", kNoCache);
  request->send(res);
}

bool WifiManager::authed(AsyncWebServerRequest* request) {
  if (request->authenticate(ADMIN_USER, settings.admin().password)) return true;
  request->requestAuthentication();
  return false;
}

// Run a request handler and survive what it throws, answering 503 instead of
// taking the node down. Diag.h explains why this has to exist at all; this is
// the single copy of it, so the POST table and the GET routes cannot drift
// into answering the same failure two different ways.
//
// The apology gets its own guard: replying is itself an allocation — a
// JsonDocument, a String, a response object — on the same exhausted heap that
// just threw, on the framework's task. A second failure here would reach the
// terminate handler and abort, which is the thing being avoided. If even this
// cannot be built the caller times out, which is a worse answer than 503 and a
// far better one than a reboot.
template <typename F>
static void serveGuarded(const char* what, AsyncWebServerRequest* r,
                         const char* apology, F&& body) {
  if (Diag::guard(what, body)) return;
  Diag::guard("the reply to a failed request", [&] { sendError(r, 503, apology); });
}

// One byte carried on the firmware-update request, freed with it: absent means
// the body handler never ran, zero that it ran and said nothing yet, and
// kUpdateAnswered that it has already replied. AsyncWebServerRequest::send()
// *replaces* a response that has not gone out rather than refusing, so a reply
// from the request handler would silently overwrite the 401 or 409 the body
// handler put there — and a browser would never see the auth challenge it was
// sent. `create` only on the first chunk; a failed allocation is not fatal,
// it just costs the early answer.
static constexpr uint8_t kUpdateAnswered = 1;
static uint8_t* updateMark(AsyncWebServerRequest* r, bool create) {
  if (!r->_tempObject && create) r->_tempObject = calloc(1, 1);
  return static_cast<uint8_t*>(r->_tempObject);
}

// ---------------------------------------------------------------------------
void WifiManager::setupRoutes() {
  // Handlers are matched in registration order: API and the protected
  // page first, then the static handler for everything else in LittleFS.
  // Guarded like the POST table below. These build the largest documents the
  // node produces, on the framework's task, and an allocation failure in one
  // of them used to abort — a browser opening the dashboard rebooted a
  // Wireless Paper three times running.
  _http.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* r) {
    serveGuarded("/api/status", r, kLowMemoryMsg, [&] { handleStatus(r); });
  });

  _http.on("/api/board", HTTP_GET, [this](AsyncWebServerRequest* r) {
    serveGuarded("/api/board", r, kLowMemoryMsg, [&] { handleBoardGet(r); });
  });

  // Messages are what somebody wrote to this node. Behind the admin password
  // like the settings page, and for the same reason: the portal is open to
  // whoever can reach the access point, and what a node was told is not
  // theirs to read.
  _http.on("/api/messages", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    serveGuarded("/api/messages", r, kLowMemoryMsg, [&] { handleMessages(r); });
  });
  _http.on("/messages.html", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    sendPage(r, "/messages.html");
  });

  // QR codes as SVG. "wifi" embeds the AP password, so it needs the admin
  // credentials like every other place that reveals it; the portal URL and
  // the node address are public.
  _http.on("/api/qr", HTTP_GET, [this](AsyncWebServerRequest* r) {
    Qr::Payload what;
    if (!Qr::parsePayload(r->hasParam("what") ? r->getParam("what")->value().c_str() : "wifi", what)) {
      sendError(r, 400, "what must be wifi, portal or address"); return;
    }
    if (what == Qr::Payload::Wifi && settings.wifi().security != ApSecurity::Open && !authed(r)) return;
    serveGuarded("/api/qr", r, kLowMemoryMsg, [&] { handleQrFor(r, what); });
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
    sendPage(r, "/settings.html");
  });
  _http.on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    serveGuarded("/api/settings", r, kLowMemoryMsg, [&] { handleSettingsGet(r); });
  });

  // A firmware bundle, posted as the raw body rather than a form: the file is
  // most of two megabytes and multipart would have the node parsing a boundary
  // out of it. Chunks go straight to the card as they land, so nothing here
  // ever holds more than one of them.
  //
  // The refusal is both decided *and answered* on the first chunk. Deciding it
  // there and replying from the request handler would still have a node with no
  // card in it spend two minutes of somebody's afternoon receiving an update it
  // was never going to take — the reply does not leave until the body has
  // arrived. The marker below is how the request handler knows the body handler
  // has already spoken; without it the 401 or 409 sent here is replaced by
  // whatever the request handler sends next.
  _http.on("/api/system/update", HTTP_POST,
           [this](AsyncWebServerRequest* r) {
             const uint8_t* mark = updateMark(r, false);
             // Nothing reached the body handler. It is not called for a request
             // the server took to be a form, so an update posted without a
             // content type is read into memory as parameters and never
             // reaches the card — which looks like a node that accepted an
             // update and then did nothing with it.
             if (!mark) {
               sendError(r, 415, "send the bundle as the raw request body with "
                                 "Content-Type: application/octet-stream");
               return;
             }
             if (*mark == kUpdateAnswered) return;      // already refused, with its own reason
             serveGuarded("/api/system/update", r, kLowMemoryMsg, [&] {
               const Ota::Progress p = Ota::progress();
               JsonDocument doc;
               doc["ok"] = p.stage != Ota::Stage::Failed;
               doc["stage"] = Ota::describe(p.stage);
               doc["message"] = p.message;
               sendJson(r, p.stage == Ota::Stage::Failed ? 400 : 202, doc);
             });
           },
           nullptr,
           [this](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
             // Marked before anything can answer, so the request handler knows
             // the body reached here at all — including on the paths below that
             // reply and stop.
             uint8_t* mark = updateMark(r, i == 0);
             auto answered = [&](int code, const char* why) {
               if (mark) *mark = kUpdateAnswered;
               if (why) sendError(r, code, why);
             };
             if (i == 0) {
               if (!authed(r)) { answered(401, nullptr); return; }   // 401 already sent
               // The gate every other privileged POST is behind: a restart is
               // already armed, and an install that starts now is one the node
               // reboots out from under.
               if (Bootloader::pending()) { answered(503, kRestartingMsg); return; }
               if (const char* why = Ota::receiveStart((uint32_t)t, r)) { answered(409, why); return; }
             }
             if (!Ota::receiveChunk(r, (uint32_t)i, d, l)) {
               // The first chunk is where a bundle that is not one is caught.
               // Later chunks have nobody left to tell: the transfer is down
               // and the request handler reports what happened to it.
               if (i == 0) {
                 const Ota::Progress p = Ota::progress();
                 answered(400, p.message);
               }
               return;
             }
             if (i + l >= t) Ota::receiveEnd(r);
           });

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
    const char* path = rt.path;
    const bool gated = !rt.ungated;
    _http.on(rt.path, HTTP_POST,
             [this](AsyncWebServerRequest* r) {
               if (r->contentLength() == 0 && authed(r)) sendError(r, 400, "empty");
             }, nullptr,
             [this, fn, gated, path](AsyncWebServerRequest* r, uint8_t* d, size_t l, size_t i, size_t t) {
               const char* body = collectBody(r, d, l, i, t);
               if (!body || !authed(r)) return;
               if (gated && Bootloader::pending()) { sendError(r, 503, kRestartingMsg); return; }
               // Every settings and system write goes through here, so this is
               // the one place that has to survive a handler running out of
               // memory (Diag.h). A request that cannot be served is answered
               // 503; before this it took the node down with it, on the
               // framework's task, under whoever sent it.
               // A settings handler saves and arms its restart before it replies,
               // so a throw on the way out can follow a write that fully
               // happened; "that request failed" would be a lie the operator
               // then acts on. Hence a different apology from the GETs above.
               serveGuarded(path, r,
                            "ran out of memory while replying — the change may have "
                            "been applied; check STATUS or /api/status",
                            [&] { (this->*fn)(r, body, t); });
             });
  }
  // What this board can do about its bootloader, for tooling. No secrets:
  // the same facts are in boards.json.
  _http.on("/api/system/bootloader", HTTP_GET,
           [this](AsyncWebServerRequest* r) { handleBootloaderGet(r); });
  // Event log from the SD card (admin). ?prev=1 serves the rotated file. Not
  // registered at all on a board with no slot: the route reads the card
  // library directly, which those boards no longer build (SdCard.h).
#if HAS_SD
  _http.on("/api/sd/log", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    if (!sdCard.mounted()) { sendError(r, 404, "no card mounted"); return; }
    const char* path = r->hasParam("prev") ? SdCard::LOG_PREV_PATH : SdCard::LOG_PATH;
    if (!SD.exists(path)) { sendError(r, 404, "no log yet"); return; }
    AsyncWebServerResponse* res = r->beginResponse(SD, path, "text/plain");
    res->addHeader("Content-Disposition", "attachment; filename=\"retimesh-events.log\"");
    r->send(res);
  });
#endif

  _http.on("/api/settings/export", HTTP_GET, [this](AsyncWebServerRequest* r) {
    if (!authed(r)) return;
    serveGuarded("/api/settings/export", r, kLowMemoryMsg, [&] { handleExport(r); });
  });
  // Takes no body, so it is not in the table above — but it writes NVS,
  // so it stands behind the same gate.
  _http.on("/api/settings/reset", HTTP_POST,
           [this](AsyncWebServerRequest* r) {
             if (!authed(r)) return;
             if (Bootloader::pending()) { sendError(r, 503, kRestartingMsg); return; }
             handleReset(r);
           });

  // OS connectivity probes — a redirect (any non-204/200 answer) is what
  // makes the client OS open its captive-portal browser. Only while there
  // is an access point to be captive on — the AP actually on the air
  // (apUp()), not the stored switches: with the AP idled down or suppressed
  // and the station still up, a LAN-side probe redirected to the AP's
  // address chases 10.42.0.1 into a void and that client's OS declares the
  // whole LAN a captive portal. Registered always and judged per request
  // rather than at boot, because the AP is no longer a boot-time fact: a
  // node that boots with it off and has WIFI ON typed later gets its
  // sign-in sheet without the restart it used to need. The judged answer
  // with no AP up is the same 404 the boot-time gate produced (the probes
  // fell to onNotFound below).
  for (const char* probe : { "/generate_204", "/gen_204",
                             "/hotspot-detect.html", "/connecttest.txt",
                             "/ncsi.txt", "/canonical.html", "/success.txt" }) {
    _http.on(probe, HTTP_GET, [this](AsyncWebServerRequest* r) {
      if (apUp()) r->redirect(PORTAL_URL);
      else        r->send(404, "text/plain", "not found");
    });
  }

  // The single-page app lives in LittleFS (data/ -> `pio run -t uploadfs`).
  // Explicit routes for the two hottest paths avoid the static handler's
  // .gz / directory probes (each one logs a VFS error at debug level 3).
  _http.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { sendPage(r, "/index.html"); });
  _http.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(204); });
  // Everything else in the image — and it must revalidate too, for the same
  // reason the pages do.
  _http.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl(kNoCache);

  // Everything else (arbitrary hostnames typed by the user, probe paths
  // not listed above) also lands on the portal — when there is one, judged
  // per request by the same predicate as the probes above.
  _http.onNotFound([this](AsyncWebServerRequest* r) {
    if (apUp()) r->redirect(PORTAL_URL);
    else        r->send(404, "text/plain", "not found");
  });
}

// ---------------------------------------------------------------------------
// GET /api/status
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The stored messages, newest first.
//
// Bounded on purpose, and bounded at what a board can render rather than at
// the depth of the ring: fifty records is over ten kilobytes of JsonDocument
// and about as much again once serialised, on the heap of a board that may
// have five kilobytes free. A reader that wants all fifty pages back through
// them with ?before=, which costs it nothing.
// ---------------------------------------------------------------------------
static void addMessage(const Rns::InboxRecord& m, void* ctx) {
  JsonArray& arr = *(JsonArray*)ctx;
  JsonObject o = arr.add<JsonObject>();
  if (o.isNull()) return;                    // out of heap; the page is short, not wrong
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", m.from[i]);
  o["seq"]      = m.seq;
  o["from"]     = hex;
  o["standing"] = Rns::standingName(m.standing);
  o["via"]      = Rns::viaName(m.via);
  o["sent_at"]  = m.sentAt;
  // Only meaningful within the run that took the message in — millis() starts
  // again at every restart — so the page is told which run it was rather than
  // being handed an age that quietly lies after a reboot.
  o["boot_id"]  = m.bootId;
  o["boot_ms"]  = m.bootMs;
  o["text"]     = String(m.text, m.textLen);
}

void WifiManager::handleMessages(AsyncWebServerRequest* request) {
  size_t want = 0;
  if (!Rns::inboxPageSize(request->hasParam("n") ? request->getParam("n")->value().c_str() : nullptr,
                          want)) {
    // Refused rather than quietly replaced with the default. The console
    // refuses the same input for the same reason, from the same rule.
    char why[64];
    snprintf(why, sizeof(why), "n must be a whole number from 1 to %u", (unsigned)Rns::kInboxPageMax);
    sendError(request, 400, why);
    return;
  }
  uint32_t from = 0;                          // 0 asks the inbox for its newest
  if (const AsyncWebParameter* p = request->getParam("before")) {
    const long v = atol(p->value().c_str());
    if (v <= 1) want = 0;                     // nothing is older than the first message
    else        from = (uint32_t)v - 1;
  }

  JsonDocument doc;
  doc["address"]   = RnsTransport::lxmf().address;
  doc["stored"]    = Rns::Inbox::stored();
  doc["newest"]    = Rns::Inbox::newest();
  doc["slots"]     = Rns::kInboxSlots;        // the page shows the ring's depth; it does not assume it
  doc["boot_id"]   = Rns::Inbox::bootId();
  doc["uptime_ms"] = millis();
  JsonArray arr = doc["messages"].to<JsonArray>();
  // One open of the store and one lock for the whole page. Reading a record at
  // a time took both per record, on the task that serves every other HTTP
  // client, each one able to wait behind a write mid-erase.
  const Rns::Inbox::Page page = want ? Rns::Inbox::readPage(from, want, addMessage, &arr)
                                     : Rns::Inbox::Page{};
  // From the read itself, not recovered from the document afterwards: an add
  // that failed for want of heap left the old arithmetic indexing past the end
  // of a short array, reading zero, and reporting that there was nothing older
  // when there were thirty-eight more.
  doc["more"] = page.more;
  sendJson(request, 200, doc);
}

void WifiManager::handleStatus(AsyncWebServerRequest* request) {
  const RadioSettings& rs = settings.radio();
  JsonDocument doc;

  doc["firmware"]     = FW_NAME;
  doc["version"]      = FW_VERSION;
  // What this node is, in words a person recognises. The name a node answers
  // to is derived from its MAC, and a bench with eight of them is eight hex
  // strings; the make and model is the half a human can match to the thing on
  // the desk. It sits at the top of the document beside the firmware it runs,
  // not inside "power", where it only ever was because the PMU line was next
  // to it.
  doc["board"]        = BOARD_NAME;
  doc["ssid"]         = _ssid;
  // Reachable as <hostname>.local only while mDNS is running, which is a
  // setting and is off by default on the boards that cannot afford it. The
  // flag travels with the name so a page does not have to guess: printing
  // "<name>.local" on a node that answers nothing by that name sends an
  // operator to a browser to be told it does not exist.
  doc["hostname"]     = _hostname;
  doc["mdns"]         = settings.maintenance().mdns;
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
  // Kept, because callers read it, but no longer the retimesh.node hash: that
  // is announced by nobody and listened on by nothing, so a caller who took it
  // for an address had one that could not be reached. It is the delivery
  // address now, the same value as lxmf_address below — one field says "where
  // this node is" and the other says which aspect it is, and they agree.
  doc["destination"]  = nodeIdentity.lxmfHex();
  // The two addresses a person is given: one to message the node at, one to
  // browse it at. Both are derived from the same identity and neither is
  // guessable from the other.
  //
  // Read from the transport rather than derived, deliberately: this one is
  // empty until Reticulum is actually up, which is the honest answer to "can I
  // message this node yet".
  doc["lxmf_address"]    = RnsTransport::lxmf().address;
  doc["nomadnet_address"] = RnsTransport::nomadAddress();
  doc["uptime_s"]     = millis() / 1000;
  {
    Power::Battery b = Power::battery();
    JsonObject pw = doc["power"].to<JsonObject>();
    pw["profile"] = Power::profileName(Power::profile());
    pw["cpu_mhz"] = getCpuFrequencyMhz();
    // Read back from the driver (esp_wifi_get_ps()), not assumed from the
    // profile: the proof a profile switch actually took effect, not just
    // that one was asked for.
    pw["wifi_ps"] = Power::wifiPsName();
    // Likewise read back (esp_wifi_get_max_tx_power), not echoed from the
    // setting: the driver quantizes down to its own steps, so this can sit
    // below wifi.tx_power. Null with Wi-Fi off, like wifi_ps says "n/a".
    {
      const float txp = Power::wifiTxPowerDbm();
      if (isnan(txp)) pw["wifi_tx_dbm"] = nullptr;
      else            pw["wifi_tx_dbm"] = txp;
    }
    pw["battery_present"] = b.present;
    // Null, not false, where the board has no way to tell. A caller can then
    // say "unknown" instead of drawing a conclusion this node never reached.
    if (b.chargeKnown) pw["battery_charging"] = b.charging;
    else               pw["battery_charging"] = nullptr;
    pw["pmu"] = Pmu::model();          // "AXP192" / "AXP2101" / "none"
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
    // The byte-addressable subset, which is what a stack or buffer must come
    // from; the figures above count 32-bit-only IRAM too and so overstate what
    // is usable. See Diag.h.
    hp["dram_free"]          = h.freeDram;
    hp["dram_min_free"]      = h.minFreeDram;
    hp["dram_largest_block"] = h.largestDramBlock;
    hp["psram_free"]    = h.freePsram;

    // What has already failed to allocate, and what was contained when it did.
    // A node under memory pressure should be readable as such while it is
    // still running: before this the only evidence was a restart, after the
    // fact, with a backtrace and no context (Diag.h).
    const Diag::Faults f = Diag::faults();
    JsonObject fo = dg["faults"].to<JsonObject>();
    fo["alloc_failures"] = f.allocFailures;
    fo["contained"]      = f.caught;
    if (f.lastMs) fo["last_ms_ago"] = millis() - f.lastMs;

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
      // Down by the idle policy, which "enabled" plus "down" alone cannot
      // say: the switch is on and the AP is not on the air, and an operator
      // reading that has to know whether to flip a switch or press a button.
      // Absent when it does not apply, like the fields above.
      if (sn.type == LocalLink::Type::WifiAp && _apSuppressed) o["idle_down"] = true;
    }
  }

  // Whether this node can take an update, and how one in flight is getting on.
  // The page needs the first to decide whether to offer the control at all, and
  // asking the node rather than inferring it from the board means one answer
  // rather than two that can disagree.
  {
    const Ota::Progress p = Ota::progress();
    JsonObject up = doc["update"].to<JsonObject>();
    up["stage"] = Ota::describe(p.stage);
    if (p.expected)   { up["received"] = p.received; up["expected"] = p.expected; }
    // The floor says which old signed images this node would still accept, and
    // the slot and the message say how an update went. /api/status is public —
    // on an open access point that is anyone within radio range — and none of
    // the three is any of their business. Withheld unless the caller holds the
    // admin credentials, the same way the coordinates below are.
    if (request->authenticate(ADMIN_USER, settings.admin().password)) {
      up["floor"] = Ota::acceptedFloor();
      up["slot"]  = Ota::runningSlot();
      if (p.message[0]) up["message"] = JsonString(p.message);
    }
    const char* why = Ota::uploadRefusal(Ota::stagingReady(), Ota::canSelfUpdate(), p.stage);
    up["can_upload"] = (why == nullptr);
    if (why) up["refusal"] = why;
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
  tr["auto_mode"] = RnsTransport::modeName(settings.transport().autoMode);
  JsonObject ai = tr["autointerface"].to<JsonObject>();
  ai["enabled"] = settings.transport().autoEnabled;
  ai["online"]  = AutoInterface::enabled();
  ai["address"] = AutoInterface::localAddress();
  ai["peers"]   = AutoInterface::peerCount();
  ai["group_id"] = settings.transport().autoGroupId[0] ? settings.transport().autoGroupId : AUTOIF_GROUP_ID;
  {
    // Which peers, not just how many: when a room of nodes cannot see each
    // other, the first question is whether they have peered at all, and the
    // answer used to be a bare count.
    // Static, not new[]. This is 1.5 KB wanted as one unbroken piece on every
    // status poll, and a node that has been up for hours has a heap in pieces
    // smaller than that long before it is short of memory: the crash that sent
    // us here reported 4320 B free and a largest block of 1396. There is no
    // recovering from it either, since a failed new[] throws and nothing on
    // the request path catches, so the node aborted and rebooted every time a
    // browser opened the dashboard. In BSS the same 1.5 KB is reserved once
    // and can never fail. Safe as a static because every HTTP handler runs on
    // the one AsyncTCP service task — platformio.ini pins it with
    // CONFIG_ASYNC_TCP_RUNNING_CORE and the library runs a single one — so no
    // two handlers are ever inside this at once.
    static AutoInterface::Peer ap[AUTOIF_MAX_PEERS];
    size_t an = AutoInterface::peers(ap, AUTOIF_MAX_PEERS);
    JsonArray pl = ai["peer_list"].to<JsonArray>();
    uint32_t nowMs = millis();
    for (size_t i = 0; i < an; i++) {
      JsonObject o = pl.add<JsonObject>();
      o["address"]   = ap[i].addr;
      o["age_s"]     = (nowMs - ap[i].lastSeenMs) / 1000;
      o["datagrams"] = ap[i].datagrams;
    }
  }
  {
    // Sized for every interface Transport can hold — the radio, the clients
    // and the Auto peers. Sized for the clients alone, this listed the first
    // five and quietly left the rest out. Off the stack because that is a
    // kilobyte and a half on a task that has other things to do with it, and
    // off the heap for the reason given at the peer list above: asked for on
    // every poll, it is the size that a long-running node can no longer find
    // in one piece, and failing to find it aborted the node.
    static RnsTransport::IfaceInfo ifs[RNS_MAX_INTERFACES];
    static RnsTransport::PathInfo  ps[32];
    // Interfaces, paths and the totals that describe them, out of one pass —
    // otherwise this document can report a path_count from one refresh beside
    // rows from another, which is a status page contradicting itself.
    const RnsTransport::Snapshot snap =
        RnsTransport::snapshot(ps, 32, ifs, RNS_MAX_INTERFACES);
    const size_t k = snap.ifaceRows;
    JsonArray ia = tr["interfaces"].to<JsonArray>();
    for (size_t i = 0; i < k; i++) {
      JsonObject o = ia.add<JsonObject>();
      o["name"] = ifs[i].name; o["mode"] = ifs[i].mode; o["rx_bytes"] = ifs[i].rxb; o["tx_bytes"] = ifs[i].txb;
    }
    const size_t pk = snap.pathRows;
    tr["path_count"]     = snap.pathTotal;
    tr["interface_count"] = snap.ifaceTotal;
    // How old the reading is. A refresh that fails leaves the previous values
    // in place, and without this they are indistinguishable from current ones
    // — on exactly the node an operator is looking at to find out what is
    // wrong with it.
    tr["snapshot_age_s"] = snap.ageMs / 1000;
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
  // Several kilobytes of SVG, handed over rather than copied — this is one of
  // the two replies a starved node was seen losing whole (ownedBodyResponse).
  AsyncWebServerResponse* res = ownedBodyResponse(request, 200, "image/svg+xml", Qr::toSvg(qr));
  if (!res) { sendError(request, 503, kLowMemoryMsg); return; }
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

void WifiManager::handleBoardGet(AsyncWebServerRequest* request) {
  String out = "[]";
  if (littleFsHas(BOARD_FILE)) {
    File f = LittleFS.open(BOARD_FILE, "r");
    if (f) {
      // readString() grows a String and fails an allocation by simply stopping,
      // so a short heap hands back an empty read that looks like an empty
      // board. Keep the default rather than answer with nothing: a file that
      // exists and reads as empty is a failure, not a wiped board.
      String stored = f.readString();
      f.close();
      if (stored.length()) out = std::move(stored);
    }
  }
  // Whatever the operator wrote on the board, which is theirs to make as long
  // as they like, so it goes the same way as the other bodies.
  AsyncWebServerResponse* res = ownedBodyResponse(request, 200, "application/json", std::move(out));
  if (!res) { sendError(request, 503, kLowMemoryMsg); return; }
  request->send(res);
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

  // The board, so the page that changes this node's settings can say which
  // node it is about to change.
  doc["board"] = BOARD_NAME;

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
    // Whether this chip has a duty-cycled receive at all, so the page can hide
    // a switch that could not be honoured rather than offering one that does
    // nothing. Read-only: it is the transceiver's answer, not a setting.
    cp["rx_duty_cycle_supported"] = c.rxDutyCycle;
#if HAS_LORA_FEM
    // Which front end the boot detection found, and roughly what it adds at
    // the configured drive — the two numbers an operator needs to work out
    // what is actually leaving the antenna.
    cp["fem"]          = LoRaFem::partName();
    cp["fem_gain_db"]  = LoRaFem::gainDb(settings.radio().txDbm);
#endif
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
  radio["rx_duty_cycle"] = rs.rxDutyCycle;       // sleep the receiver between preamble samples
  // ...and what that setting is actually worth here. RadioLib falls back to a
  // continuous receive without reporting it whenever the sleep the channel
  // yields is shorter than the chip's wake-up transition, so the switch alone
  // says nothing: rx_duty_cycle_engages is the computed truth (Airtime.h) and
  // rx_duty_cycle_sleep_us the figure behind it. _would_engage is the same
  // computation with the switch left out, so a client can tell "this channel
  // cannot" from "nobody asked" — and _armed is whether the receiver is running
  // the mode at all, which is false in this release however the rest read.
  radio["rx_duty_cycle_engages"]       = g_stats.rxDutyCycleEngages;
  radio["rx_duty_cycle_would_engage"]  = g_stats.rxDutyCycleWouldEngage;
  radio["rx_duty_cycle_armed"]         = g_stats.rxDutyCycleArmed;
  radio["rx_duty_cycle_sleep_us"]      = g_stats.rxDutyCycleSleepUs;
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
  // The stored setting the form edits; the ceiling the driver actually holds
  // after quantization is the status surfaces' wifi_tx_dbm.
  wifi["tx_power"]   = ws.txPowerDbm;
  wifi["sta_ssid"]   = ws.staSsid;
  wifi["sta_has_password"] = ws.staPassword[0] != '\0';
  wifi["sta_listen_interval"] = ws.staListenInterval;
  wifi["sta_connected"] = stationConnected();
  wifi["ap_idle_off"]     = ws.apIdleOff;
  wifi["ap_idle_minutes"] = ws.apIdleMinutes;

  JsonObject tr = doc["transport"].to<JsonObject>();
  tr["enabled"]   = settings.transport().enabled;
  tr["lora_mode"] = settings.transport().loraMode;
  tr["wifi_mode"] = settings.transport().wifiMode;
  tr["auto_mode"] = settings.transport().autoMode;
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
      // The links card must be able to say "on, but down by the idle
      // policy" — a switch that reads on beside an AP nobody can see sends
      // the operator toward the wrong control.
      if (f[i].type == LocalLink::Type::WifiAp && _apSuppressed) o["idle_down"] = true;
      if (f[i].type == LocalLink::Type::PppUart && l->usable()) {
        // The speed, and the speeds this board may be set to — the page
        // draws its list from this answer, as it draws the switches, so
        // the rule (LocalLinkState.h) is applied here and nowhere on the page.
        o["baud"] = settings.links().pppBaud;
        JsonArray bauds = o["bauds"].to<JsonArray>();
        size_t nb = 0;
        const uint32_t* ladder = LocalLink::pppBauds(nb);
        for (size_t b = 0; b < nb; b++) if (LocalLink::pppBaudUsable(ladder[b])) bauds.add(ladder[b]);
        #if HAS_PPP
          // The addresses the node asks for, which a host gives its pppd.
          o["node_ip"] = PppUart::askedAddress().toString();
          o["host_ip"] = PppUart::askedPeer().toString();
        #endif
      }
    }
  }
  {
    JsonObject m = doc["maintenance"].to<JsonObject>();
    for (const MaintField& f : kMaintFields) m[f.key] = settings.maintenance().*(f.on);
    // The list is not a switch, so it is not in the table above. It is not a
    // secret either — a source hash is public, and an operator needs to read
    // back what they configured.
    m["rns_admins"] = settings.maintenance().rnsAdmins;
  }
  bootloaderJson(doc["bootloader"].to<JsonObject>());

  doc["admin"]["user"] = ADMIN_USER;
  doc["admin"]["default_password"] = strcmp(settings.admin().password, ADMIN_PASSWORD_DEFAULT) == 0;

  sendJson(request, 200, doc);
}

// ---------------------------------------------------------------------------
// Local links and maintenance settings
// ---------------------------------------------------------------------------
// POST /api/settings/links {"wifi_ap":bool,"wifi_sta":bool,"usb":bool,
// "ppp":bool,"ppp_baud":int} — any subset. A link the board lacks or the
// build cannot run is refused by name rather than saved: a setting nothing
// acts on is a lie the page would go on showing; so is a PPP speed the board
// is not qualified for. The access point's switch applies live — the tick
// convergence takes it up or down, with a grace so this very reply leaves
// first — as do USB and PPP; the station switch still restarts (the join is
// built at boot); the answer says which happened.
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
  if (in["ppp_baud"].is<uint32_t>()) want.pppBaud = in["ppp_baud"];
  const char* detail = "";
  const LocalLink::Apply a = LocalLink::applyLinks(want, changed, Bootloader::Source::Settings, &detail);
  JsonDocument out;
  switch (a) {
    case LocalLink::Apply::RefusedUnusable: {
      char msg[160];
      snprintf(msg, sizeof(msg), "cannot be enabled: %s", detail);
      sendError(request, 400, msg); return;
    }
    case LocalLink::Apply::RefusedBaud: {
      char msg[160];
      snprintf(msg, sizeof(msg), "ppp_baud %lu refused: %s", (unsigned long)want.pppBaud, detail);
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
  // Through the same rule the console's SET uses, so the two cannot disagree
  // about what a valid list is — which they already did: "-" cleared it at the
  // console and was a 400 here (SettingsFields.h).
  if (in["rns_admins"].is<const char*>()) {
    char why[128] = "";
    if (!SettingsFields::setRnsAdmins(m, in["rns_admins"], why, sizeof(why))) {
      sendError(request, 400, why);
      return;
    }
  }
  // Through the same commit the console uses, so both refuse alike and the
  // restart the portal's switch needs is asked for once rather than twice.
  char detail[160] = "";
  const SettingsFields::Result res = SettingsFields::commitMaintenance(m, detail, sizeof(detail));
  switch (res) {
    case SettingsFields::Result::Ok:
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    case SettingsFields::Result::OkRestart:
    case SettingsFields::Result::OkNextBoot:
      request->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
      return;
    // 503, as every settings POST answers while a restart is pending
    // (docs/api.md). The gate in setupRoutes answers it first, so this is
    // the same answer from the other side of it rather than a second one.
    case SettingsFields::Result::Busy:      sendError(request, 503, detail[0] ? detail : "the node is restarting"); return;
    case SettingsFields::Result::Refused:   sendError(request, 400, detail); return;
    case SettingsFields::Result::NvsFailed: sendError(request, 500, "nvs"); return;
    default:                                sendError(request, 400, detail[0] ? detail : "refused"); return;
  }
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
  // The switch and the link, asked of Bootloader so the network console
  // refuses on exactly these terms too (Bootloader.h).
  const char* denied = nullptr;
  if (!Bootloader::remoteEntryAllowed(fromHostFacingLink(request), &denied)) {
    sendError(request, 403, denied); return;
  }
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || strcmp(in["confirm"] | "", "BOOTLOADER") != 0) {
    sendError(request, 400, "send {\"confirm\":\"BOOTLOADER\"} to restart into the ROM downloader"); return;
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
  // The integers go through jsonNarrow (the helper says why): an out-of-width
  // value used to arrive at validateRadio as 0, and for the fields where 0 is
  // legal — the sync word, 0 dBm on a transceiver whose floor is negative,
  // "off" for the intervals and the duty cycle — it was stored, silently.
  if (!jsonNarrow(request, in["sf"], r.sf, "sf")) return;
  if (!jsonNarrow(request, in["cr"], r.cr, "cr")) return;
  if (!jsonNarrow(request, in["tx_dbm"], r.txDbm, "tx_dbm")) return;
  if (!jsonNarrow(request, in["sync_word"], r.syncWord, "sync_word")) return;
  if (!jsonNarrow(request, in["preamble"], r.preamble, "preamble")) return;
  if (!jsonNarrow(request, in["beacon_interval"], r.beaconInterval, "beacon_interval")) return;
  if (!jsonNarrow(request, in["announce_interval"], r.announceInterval, "announce_interval")) return;
  if (!jsonNarrow(request, in["duty_cycle_pct"], r.dutyCyclePct, "duty_cycle_pct")) return;
  if (in["rx_duty_cycle"].is<bool>()) r.rxDutyCycle  = in["rx_duty_cycle"];
  if (in["gps_enabled"].is<bool>())   r.gpsEnabled   = in["gps_enabled"];
  if (in["gps_share_position"].is<bool>()) r.gpsSharePosition = in["gps_share_position"];
  char msg0[160];
  if (in["callsign"].is<const char*>()) {
    String c = in["callsign"].as<String>(); c.trim();
    // Checked before the copy: the field is fixed-width, so a check after it
    // would see a truncated callsign and pass.
    if (!SettingsRules::validateCallsign(c.c_str(), msg0, sizeof(msg0))) { sendError(request, 400, msg0); return; }
    strlcpy(r.callsign, c.c_str(), sizeof(r.callsign));
  }
  if (in["region"].is<const char*>()) {
    strlcpy(r.region, in["region"].as<const char*>(), sizeof(r.region));
  }

  // Every bound is SettingsRules', so the console refuses the same value with
  // the same words (SettingsRules.h).
  char msg[160];
  const int8_t maxDbm = loraRadio.online() ? loraRadio.maxTxDbm() : 22;
  if (!SettingsRules::validateRadio(r, loraRadio.caps(), maxDbm, msg, sizeof(msg))) {
    sendError(request, 400, msg); return;
  }

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
  // The integers go through jsonNarrow — read wide, refused when the field's
  // width would change them — so validateWifi below always judges the number
  // that was sent (the helper says why the plain assignment could not be
  // trusted to deliver it).
  if (!jsonNarrow(request, in["channel"], w.channel, "channel")) return;
  if (!jsonNarrow(request, in["max_stations"], w.maxStations, "max_stations")) return;
  if (in["hidden"].is<bool>()) w.hidden = in["hidden"];
  if (!jsonNarrow(request, in["tx_power"], w.txPowerDbm, "tx_power")) return;
  if (!jsonNarrow(request, in["sta_listen_interval"], w.staListenInterval, "sta_listen_interval")) return;
  if (in["ap_idle_off"].is<bool>()) w.apIdleOff = in["ap_idle_off"];
  if (!jsonNarrow(request, in["ap_idle_minutes"], w.apIdleMinutes, "ap_idle_minutes")) return;
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

  // Through the same commit the console uses (SettingsFields.h): it holds
  // the one copy of the live/restart split — the TX ceiling and the station
  // listen interval apply now, and everything the access point or the join
  // is built from asks for the restart. The restart is asked for before the
  // answer is sent, so the answer can say whether it was granted; the
  // bootloader's delay is what lets the reply leave, not the order here.
  char wmsg[160] = "";
  const SettingsFields::Result res = SettingsFields::commitWifi(w, wmsg, sizeof(wmsg));
  switch (res) {
    case SettingsFields::Result::Ok:
    case SettingsFields::Result::OkRestart:
    case SettingsFields::Result::OkNextBoot: {
      JsonDocument out;
      out["ok"] = true;
      out["restart"] = (res == SettingsFields::Result::OkRestart);
      // "restart": false alone covers two shapes an operator must tell
      // apart — only the live fields changed (applied now), or the restart
      // was needed and not granted. The note is the distinction, in the
      // same words the links endpoint uses for the same state.
      if (res == SettingsFields::Result::OkNextBoot)
        out["note"] = "saved; a restart is already in progress, so the change applies at the next boot";
      out["ssid"] = w.ssid[0] ? w.ssid : _ssid;   // auto-derived name does not change
      out["security"] = Settings::securityName(w.security);
      sendJson(request, 200, out);
      return;
    }
    case SettingsFields::Result::Busy:      sendError(request, 503, wmsg[0] ? wmsg : kRestartingMsg); return;
    case SettingsFields::Result::NvsFailed: sendError(request, 500, "nvs"); return;
    default:                                sendError(request, 400, wmsg[0] ? wmsg : "refused"); return;
  }
}

void WifiManager::handleAdminPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok || !in["password"].is<const char*>()) {
    sendError(request, 400, "bad json"); return;
  }
  const char* p = in["password"];
  char amsg[160];
  if (!SettingsRules::validateAdminPassword(p, amsg, sizeof(amsg))) { sendError(request, 400, amsg); return; }
  if (!settings.saveAdminPassword(p)) { sendError(request, 500, "nvs"); return; }
  request->send(200, "application/json", "{\"ok\":true}");
}

void WifiManager::handleTransportPost(AsyncWebServerRequest* request, const char* body, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, body, len) != DeserializationError::Ok) { sendError(request, 400, "bad json"); return; }
  TransportSettings t = settings.transport();
  if (in["enabled"].is<bool>())  t.enabled  = in["enabled"];
  // The integers go through jsonNarrow — the helper says why the plain
  // assignment delivered a different number than the one sent.
  if (!jsonNarrow(request, in["lora_mode"], t.loraMode, "lora_mode")) return;
  if (!jsonNarrow(request, in["wifi_mode"], t.wifiMode, "wifi_mode")) return;
  if (!jsonNarrow(request, in["auto_mode"], t.autoMode, "auto_mode")) return;
  if (!jsonNarrow(request, in["announce_cap"], t.announceCap, "announce_cap")) return;
  if (!jsonNarrow(request, in["announce_rate_target"], t.announceRateTarget, "announce_rate_target")) return;
  if (!jsonNarrow(request, in["announce_rate_grace"], t.announceRateGrace, "announce_rate_grace")) return;
  if (!jsonNarrow(request, in["announce_rate_penalty"], t.announceRatePenalty, "announce_rate_penalty")) return;
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
  char tmsg[160];
  if (!SettingsRules::validateTransport(t, tmsg, sizeof(tmsg))) { sendError(request, 400, tmsg); return; }
  TransportSettings before = settings.transport();
  if (!settings.saveTransport(t)) { sendError(request, 500, "nvs"); return; }
  Power::apply((Power::Profile)t.powerProfile);                  // live
  bool needRestart = before.enabled != t.enabled || before.loraMode != t.loraMode || before.wifiMode != t.wifiMode
                  || before.autoMode != t.autoMode
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
  r["rx_duty_cycle"] = rs.rxDutyCycle;
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["ssid"] = ws.ssid; w["security"] = Settings::securityName(ws.security); w["password"] = ws.password;
  w["channel"] = ws.channel; w["max_stations"] = ws.maxStations; w["hidden"] = ws.hidden;
  w["tx_power"] = ws.txPowerDbm; w["sta_listen_interval"] = ws.staListenInterval;
  w["ap_idle_off"] = ws.apIdleOff; w["ap_idle_minutes"] = ws.apIdleMinutes;
  w["sta_ssid"] = ws.staSsid; w["sta_password"] = ws.staPassword;
  JsonObject t = doc["transport"].to<JsonObject>();
  t["enabled"] = ts.enabled; t["lora_mode"] = ts.loraMode; t["wifi_mode"] = ts.wifiMode;
  t["auto_mode"] = ts.autoMode;
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
    const LocalLink::Link* ppp = LocalLink::find(LocalLink::Type::PppUart);
    if (ppp && ppp->usable()) l["ppp_baud"] = settings.links().pppBaud;
  }
  JsonObject m = doc["maintenance"].to<JsonObject>();
  for (const MaintField& f : kMaintFields) m[f.key] = settings.maintenance().*(f.on);
  doc["admin"]["password"] = settings.admin().password;
  String out;
  if (!serializedWhole(doc, out, true)) {          // the biggest document here
    sendError(request, 503, kLowMemoryMsg);
    return;
  }
  AsyncWebServerResponse* res = ownedBodyResponse(request, 200, "application/json", std::move(out));
  if (!res) {
    sendError(request, 503, kLowMemoryMsg);
    return;
  }
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
    rs.freqMhz = r["freq_mhz"] | rs.freqMhz; rs.bwKhz = r["bw_khz"] | rs.bwKhz;
    // Integers through jsonNarrow, as on the POST paths (the helper says
    // why): here the `|` fallback made an out-of-width value silently keep
    // the stored one — a mangled file answered ok with parts of it dropped.
    if (!jsonNarrow(request, r["sf"], rs.sf, "sf")) return;
    if (!jsonNarrow(request, r["cr"], rs.cr, "cr")) return;
    if (!jsonNarrow(request, r["tx_dbm"], rs.txDbm, "tx_dbm")) return;
    if (!jsonNarrow(request, r["sync_word"], rs.syncWord, "sync_word")) return;
    if (!jsonNarrow(request, r["preamble"], rs.preamble, "preamble")) return;
    if (!jsonNarrow(request, r["announce_interval"], rs.announceInterval, "announce_interval")) return;
    if (!jsonNarrow(request, r["beacon_interval"], rs.beaconInterval, "beacon_interval")) return;
    if (r["callsign"].is<const char*>()) strlcpy(rs.callsign, r["callsign"], sizeof(rs.callsign));
    // Carried across whatever chip the receiving node has: the mode is refused
    // by nothing and simply does not engage where it cannot (SettingsRules.h),
    // so one export provisions a mixed fleet without being edited per board.
    rs.rxDutyCycle = r["rx_duty_cycle"] | rs.rxDutyCycle;
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
    // Integers through jsonNarrow, as on the POST path (the radio section
    // above says why the `|` fallback could not be kept for them).
    if (!jsonNarrow(request, w["channel"], ws.channel, "channel")) return;
    if (!jsonNarrow(request, w["max_stations"], ws.maxStations, "max_stations")) return;
    ws.hidden = w["hidden"] | ws.hidden;
    if (!jsonNarrow(request, w["tx_power"], ws.txPowerDbm, "tx_power")) return;
    if (!jsonNarrow(request, w["sta_listen_interval"], ws.staListenInterval, "sta_listen_interval")) return;
    ws.apIdleOff = w["ap_idle_off"] | ws.apIdleOff;
    if (!jsonNarrow(request, w["ap_idle_minutes"], ws.apIdleMinutes, "ap_idle_minutes")) return;
    if (ws.security != ApSecurity::Open && strlen(ws.password) < 8) ws.security = ApSecurity::Open;
    // The unusable password goes with the coercion, whichever import wrote
    // it: kept, it fails validateWifi's 8-63 rule below, turning files that
    // imported fine before this validation existed — and self-exports of
    // nodes the coercion above already ran on, which say "open" with the
    // short password still in them — into 400s. Cleared, the section means
    // exactly what the node will run: an open network.
    if (ws.password[0] != '\0' && strlen(ws.password) < 8) ws.password[0] = '\0';
    // The shared rule, not a local copy of its bounds: the inline check that
    // sat here knew channel and max_stations and would have silently waved
    // every later field through — the drift SettingsRules exists to prevent.
    char wmsg[160];
    if (!SettingsRules::validateWifi(ws, wmsg, sizeof(wmsg))) { sendError(request, 400, wmsg); return; }
    settings.saveWifi(ws);
  }
  bool storeHomeIgnored = false;
  bool mdnsTurnedOn = false;
  if (in["transport"].is<JsonObject>()) {
    JsonObject t = in["transport"]; TransportSettings ts = settings.transport();
    ts.enabled = t["enabled"] | ts.enabled;
    // Integers through jsonNarrow, as on the POST path (the radio section
    // above says why the `|` fallback could not be kept for them).
    if (!jsonNarrow(request, t["lora_mode"], ts.loraMode, "lora_mode")) return;
    if (!jsonNarrow(request, t["wifi_mode"], ts.wifiMode, "wifi_mode")) return;
    if (!jsonNarrow(request, t["auto_mode"], ts.autoMode, "auto_mode")) return;
    if (!jsonNarrow(request, t["announce_cap"], ts.announceCap, "announce_cap")) return;
    if (!jsonNarrow(request, t["announce_rate_target"], ts.announceRateTarget, "announce_rate_target")) return;
    if (!jsonNarrow(request, t["announce_rate_grace"], ts.announceRateGrace, "announce_rate_grace")) return;
    if (!jsonNarrow(request, t["announce_rate_penalty"], ts.announceRatePenalty, "announce_rate_penalty")) return;
    ts.autoEnabled = t["auto_enabled"] | ts.autoEnabled;
    // Not imported, and not a reason to refuse the file either. Where the store
    // lives describes the node the backup came from — whether that one had a
    // card in its slot — and not the configuration being restored. Restoring
    // onto a replacement node is exactly when this differs and exactly when
    // failing the whole import is least welcome, so the field is dropped and
    // the answer says so. The store is moved with the card actions, which copy
    // the data; setting the flag alone never did.
    storeHomeIgnored = t["sd_store"].is<bool>() && (bool)t["sd_store"] != ts.sdStore;
    if (!jsonNarrow(request, t["power_profile"], ts.powerProfile, "power_profile")) return;
    if (t["auto_group_id"].is<const char*>()) strlcpy(ts.autoGroupId, t["auto_group_id"], sizeof(ts.autoGroupId));
    // The shared rule, not a local copy of its bounds: the inline check that
    // sat here knew the modes and the cap and silently waved auto_group_id
    // and power_profile through — the drift SettingsRules exists to prevent
    // (the wifi section above tells the same story).
    char tmsg[160];
    if (!SettingsRules::validateTransport(ts, tmsg, sizeof(tmsg))) { sendError(request, 400, tmsg); return; }
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
      // The PPP speed, where this board runs PPP and is qualified for it;
      // dropped otherwise, as a switch for a link this board lacks is —
      // the file came from another node.
      if (lk["ppp_baud"].is<uint32_t>()) {
        const uint32_t baud = lk["ppp_baud"];
        const LocalLink::Link* ppp = LocalLink::find(LocalLink::Type::PppUart);
        if (ppp && ppp->usable() && LocalLink::pppBaudUsable(baud)) ls.pppBaud = baud;
      }
    }
    if (haveMaint) {
      JsonObject mt = in["maintenance"];
      const bool mdnsWas = ms.mdns;
      for (const MaintField& f : kMaintFields) ms.*(f.on) = mt[f.key] | ms.*(f.on);
      // A file exported from a roomier board carries mDNS on, and this board
      // may be one that starts without it for a reason (Config.h). The import
      // is the operator's own act so it is honoured — but not silently, since
      // the memory it costs is the memory this board did not have.
      if (ms.mdns && !mdnsWas && !MDNS_ENABLED_DEFAULT)
        mdnsTurnedOn = true;
    }
    if ((haveLinks || haveMaint) && LocalLink::lockedOut(ls, ms.consoleEnabled, ms.webUi)) {
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
  else if (mdnsTurnedOn)
    out["note"] = "this file switched mDNS on, which this board starts without: it costs about 6 KB of "
                  "byte-addressable RAM, and this board has little to spare (maintenance.mdns turns it off again)";
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
