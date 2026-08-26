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
//  RnsTransport.cpp — see RnsTransport.h
// ============================================================================
#include "RnsTransport.h"
#include <microReticulum.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <map>
#include <vector>
#include "RnsFileSystem.h"
#include "RnsAnnounce.h"
#include "Settings.h"
#include "LoRaRadio.h"
#include "RetiTransportServer.h"
#include "WifiManager.h"

using RNS::Bytes;

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Identity    nodeRnsIdentity({RNS::Type::NONE});
static RNS::Destination nodeDest({RNS::Type::NONE});
static RnsFileSystem    rnsFs;
static bool             sStarted = false;

static RingbufHandle_t  sTxRing = nullptr, sRxRing = nullptr, sTcpInRing = nullptr;
static QueueHandle_t    sEvents = nullptr;
static SemaphoreHandle_t sSnapLock = nullptr;
static uint32_t         sNextAnnounceMs = 0;

static RNS::Type::Interface::modes toMode(uint8_t m) {
  using namespace RNS::Type::Interface;
  switch (m) {
    case 2:  return MODE_GATEWAY;
    case 3:  return MODE_ACCESS_POINT;
    case 4:  return MODE_ROAMING;
    case 5:  return MODE_BOUNDARY;
    default: return MODE_FULL;
  }
}

const char* RnsTransport::modeName(uint8_t m) {
  switch (m) {
    case 2: return "gateway";
    case 3: return "access_point";
    case 4: return "roaming";
    case 5: return "boundary";
    default: return "full";
  }
}

static const char* modeNameOf(RNS::Type::Interface::modes m) {
  using namespace RNS::Type::Interface;
  switch (m) {
    case MODE_GATEWAY:      return "gateway";
    case MODE_ACCESS_POINT: return "access_point";
    case MODE_ROAMING:      return "roaming";
    case MODE_BOUNDARY:     return "boundary";
    case MODE_POINT_TO_POINT: return "point_to_point";
    default:                return "full";
  }
}

// ---------------------------------------------------------------------------
// LoRa interface: rings shared with radioTask (see LoRaRadio.h)
// ---------------------------------------------------------------------------
class LoRaRnsInterface : public RNS::InterfaceImpl {
public:
  LoRaRnsInterface() : RNS::InterfaceImpl("LoRa") {
    _IN = _OUT = true;
    _HW_MTU = RNS_MTU;
    const RadioSettings& r = settings.radio();
    _bitrate = (uint32_t)(r.sf * ((4.0 / r.cr) / (pow(2.0, r.sf) / r.bwKhz)) * 1000.0);
  }
  bool send_outgoing(const Bytes& data) override {
    if (!g_stats.radioOnline) return false;
    if (xRingbufferSend(sTxRing, data.data(), data.size(), pdMS_TO_TICKS(20)) != pdTRUE) {
      log_w("LoRa TX ring full, dropping %u bytes", (unsigned)data.size());
      return false;
    }
    return true;
  }
  void loop() override {
    size_t sz = 0;
    uint8_t* item;
    while ((item = (uint8_t*)xRingbufferReceive(sRxRing, &sz, 0)) != nullptr) {
      // Signal stats are private to InterfaceImpl; the wrapper exposes setters.
      std::shared_ptr<RNS::InterfaceImpl> sp = shared_from_this();
      RNS::Interface self(sp);
      self.r_stat_rssi(g_stats.lastRssi);
      self.r_stat_snr(g_stats.lastSnr);
      if (Rns::isAnnounce(item, sz)) RetiTransportServer::noteAnnounce(item, sz, false);
      handle_incoming(Bytes(item, sz));
      vRingbufferReturnItem(sRxRing, item);
    }
  }
};

// ---------------------------------------------------------------------------
// One interface per Wi-Fi client
// ---------------------------------------------------------------------------
class TcpClientRnsInterface : public RNS::InterfaceImpl {
public:
  TcpClientRnsInterface(uint32_t id, const char* name) : RNS::InterfaceImpl(name), _id(id) {
    _IN = _OUT = true;
    _HW_MTU = RNS_MTU;
    _bitrate = 10000000;                    // Wi-Fi; only used for airtime maths
  }
  bool send_outgoing(const Bytes& data) override {
    return transportServer.sendTo(_id, data.data(), data.size());
  }
  void incoming(const uint8_t* p, size_t len) {
    if (Rns::isAnnounce(p, len)) RetiTransportServer::noteAnnounce(p, len, true);
    handle_incoming(Bytes(p, len));
  }
private:
  uint32_t _id;
};

static RNS::Interface loraIface({RNS::Type::NONE});
struct TcpIface { RNS::Interface handle; TcpClientRnsInterface* impl; };
static std::map<uint32_t, TcpIface> tcpIfaces;

struct Event { bool connect; uint32_t id; char remote[24]; };

// ---------------------------------------------------------------------------
namespace RnsTransport {

bool started() { return sStarted; }

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing) {
  sTxRing = txRing; sRxRing = rxRing; sTcpInRing = tcpInRing;
  sEvents = xQueueCreate(8, sizeof(Event));
  sSnapLock = xSemaphoreCreateMutex();
  if (!settings.transport().enabled) { log_w("Reticulum transport disabled in settings"); return false; }

  try {
    RNS::loglevel(RNS::LOG_INFO);
    rnsFs.init(false);
    RNS::Utilities::OS::register_filesystem(rnsFs);
    reticulum = RNS::Reticulum();          // ctor resets the storage path, so set it after
    RNS::Reticulum::storagepath(RNS_FS_ROOT);
    reticulum.transport_enabled(true);

    // One identity for everything: transport signs with the same keys that
    // announce retimesh.node, so Transport::identity() == nodeIdentity.
    uint8_t prv[64];
    nodeIdentity.privateKey(prv);
    Bytes prvBytes(prv, sizeof(prv));
    RNS::Transport::set_identity_prv(prvBytes);
    nodeRnsIdentity = RNS::Identity(false);
    nodeRnsIdentity.load_private_key(prvBytes);

    loraIface = RNS::Interface(new LoRaRnsInterface());
    loraIface.mode(toMode(settings.transport().loraMode));
    RNS::Transport::register_interface(loraIface);
    loraIface.start();

    reticulum.start();

    nodeDest = RNS::Destination(nodeRnsIdentity, RNS::Type::Destination::IN,
                                RNS::Type::Destination::SINGLE, "retimesh", "node");
    sStarted = true;
    sNextAnnounceMs = millis() + ANNOUNCE_BOOT_DELAY_MS;
    log_i("Reticulum transport up: identity %s, LoRa mode %s, Wi-Fi clients %s",
          RNS::Transport::identity().hash().toHex().c_str(),
          modeName(settings.transport().loraMode), modeName(settings.transport().wifiMode));
  } catch (const std::exception& e) {
    log_e("Reticulum transport start failed: %s", e.what());
  }
  return sStarted;
}

void clientConnected(uint32_t id, const char* remote) {
  Event e{ true, id, {0} };
  strlcpy(e.remote, remote, sizeof(e.remote));
  xQueueSend(sEvents, &e, 0);
}

void clientDisconnected(uint32_t id) {
  Event e{ false, id, {0} };
  xQueueSend(sEvents, &e, 0);
}

static void processEvents() {
  Event e;
  while (xQueueReceive(sEvents, &e, 0) == pdTRUE) {
    if (e.connect) {
      char name[32];
      snprintf(name, sizeof(name), "WiFi/%s", e.remote);
      auto* impl = new TcpClientRnsInterface(e.id, name);
      RNS::Interface iface(impl);
      iface.mode(toMode(settings.transport().wifiMode));
      RNS::Transport::register_interface(iface);
      iface.start();
      tcpIfaces.emplace(e.id, TcpIface{ iface, impl });
      log_i("registered %s (%s)", name, modeName(settings.transport().wifiMode));
    } else {
      auto it = tcpIfaces.find(e.id);
      if (it != tcpIfaces.end()) {
        RNS::Transport::deregister_interface(it->second.handle);
        tcpIfaces.erase(it);
        log_i("deregistered Wi-Fi client %lu", (unsigned long)e.id);
      }
    }
  }
}

static void drainTcp() {
  size_t sz = 0;
  uint8_t* item;
  while ((item = (uint8_t*)xRingbufferReceive(sTcpInRing, &sz, 0)) != nullptr) {
    if (sz > sizeof(TcpItemHeader)) {
      TcpItemHeader h; memcpy(&h, item, sizeof(h));
      auto it = tcpIfaces.find(h.clientId);
      if (it != tcpIfaces.end()) it->second.impl->incoming(item + sizeof(h), sz - sizeof(h));
    }
    vRingbufferReturnItem(sTcpInRing, item);
  }
}

// Snapshots for the web layer
static std::vector<PathInfo>  sPaths;
static std::vector<IfaceInfo> sIfaces;
static uint32_t sSnapAtMs = 0;

static void refreshSnapshots() {
  if (millis() - sSnapAtMs < 2000) return;
  sSnapAtMs = millis();
  std::vector<PathInfo> p;
  std::vector<IfaceInfo> i;
  double now = RNS::Utilities::OS::time();
  for (const auto& kv : RNS::Transport::path_table()) {
    PathInfo pi = {};
    strlcpy(pi.hash, kv.first.toHex().c_str(), sizeof(pi.hash));
    auto& de = const_cast<RNS::Persistence::DestinationEntry&>(kv.second);
    pi.hops = de._hops;
    strlcpy(pi.via, de.receiving_interface() ? de.receiving_interface().name().c_str() : "?", sizeof(pi.via));
    pi.ageS = (uint32_t)max(0.0, now - de._timestamp);
    p.push_back(pi);
    if (p.size() >= 64) break;
  }
  for (const auto& iface : RNS::Transport::get_interfaces()) {
    IfaceInfo ii = {};
    strlcpy(ii.name, iface.name().c_str(), sizeof(ii.name));
    strlcpy(ii.mode, modeNameOf(iface.mode()), sizeof(ii.mode));
    ii.rxb = iface.rxbytes(); ii.txb = iface.txbytes();
    i.push_back(ii);
  }
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  sPaths.swap(p); sIfaces.swap(i);
  xSemaphoreGive(sSnapLock);
}

size_t paths(PathInfo* out, size_t max) {
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  size_t n = min(max, sPaths.size());
  for (size_t k = 0; k < n; k++) out[k] = sPaths[k];
  xSemaphoreGive(sSnapLock);
  return n;
}

size_t interfaces(IfaceInfo* out, size_t max) {
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  size_t n = min(max, sIfaces.size());
  for (size_t k = 0; k < n; k++) out[k] = sIfaces[k];
  xSemaphoreGive(sSnapLock);
  return n;
}

size_t pathCount() {
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  size_t n = sPaths.size();
  xSemaphoreGive(sSnapLock);
  return n;
}

void loop() {
  if (!sStarted) { vTaskDelay(pdMS_TO_TICKS(500)); return; }
  try {
    processEvents();
    drainTcp();
    reticulum.loop();                      // Transport jobs + every interface's loop()

    uint16_t interval = settings.radio().announceInterval;
    if (interval && sNextAnnounceMs && (int32_t)(millis() - sNextAnnounceMs) >= 0) {
      char app[64];
      int n = snprintf(app, sizeof(app), "%s %s", loraRadio.callsign(), FW_VERSION);
      nodeDest.announce(Bytes((const uint8_t*)app, (size_t)max(n, 0)));
      g_stats.announcesTx++;
      sNextAnnounceMs = millis() + (uint32_t)interval * 1000UL;
      log_i("announced retimesh.node <%s> on all interfaces", nodeIdentity.destHex());
    }
    refreshSnapshots();
  } catch (const std::exception& e) {
    log_e("Reticulum loop: %s", e.what());
  }
}

} // namespace RnsTransport
