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
#include "StoreHome.h"
#include "Neighbors.h"
#include "SdCard.h"
#include "RnsAnnounce.h"
#include "Settings.h"
#include "LoRaRadio.h"
#include "RetiTransportServer.h"
#include "WifiManager.h"
#include "AutoInterface.h"

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

// One line per store directory at boot ("path_store: 2 files, 12 KB").
// It is the quickest way to tell whether the tables survived a restart —
// an empty list after an uptime with traffic means the store is not
// persisting, on either backend.
static void logStoreContents() {
  fs::FS* fs = RnsFileSystem::backend();
  fs::File root = fs->open(RNS_FS_ROOT);
  if (!root || !root.isDirectory()) { log_i("store %s: empty", RnsTransport::storagePath()); return; }
  for (fs::File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const char* path = entry.path() ? entry.path() : entry.name();
    if (entry.isDirectory()) {
      unsigned files = 0; unsigned long bytes = 0;
      fs::File sub = fs->open(path);
      for (fs::File f = sub.openNextFile(); f; f = sub.openNextFile()) { files++; bytes += f.size(); f.close(); }
      sub.close();
      if (files) log_i("store %s: %u files, %lu KB", path, files, bytes / 1024);
    } else if (entry.size()) {
      log_i("store %s: %lu B", path, (unsigned long)entry.size());
    }
    entry.close();
  }
  root.close();
}

const char* RnsTransport::storageBackend() { return RnsFileSystem::backendName(); }

const char* RnsTransport::storagePath() {
  static char path[32];
  snprintf(path, sizeof(path), "%s%s", RnsFileSystem::prefix(), RNS_FS_ROOT);
  return path;
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
static void applyAnnounceLimits(RNS::InterfaceImpl& impl);

class LoRaRnsInterface : public RNS::InterfaceImpl {
public:
  LoRaRnsInterface() : RNS::InterfaceImpl("LoRa") {
    _IN = _OUT = true;
    _HW_MTU = RNS_MTU;
    applyAnnounceLimits(*this);
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
    applyAnnounceLimits(*this);
    _bitrate = 10000000;                    // Wi-Fi; only used for airtime maths
  }
  bool send_outgoing(const Bytes& data) override {
    if (_id & AutoInterface::AUTO_ID_BASE) return AutoInterface::sendTo(_id, data.data(), data.size());
    return transportServer.sendTo(_id, data.data(), data.size());
  }
  void incoming(const uint8_t* p, size_t len) {
    handle_incoming(Bytes(p, len));
  }
private:
  uint32_t _id;
};

// The limits live in protected InterfaceImpl fields; a tiny subclass view
// lets one helper set them for every interface type.
struct LimitsView : RNS::InterfaceImpl {
  void apply(const TransportSettings& t) {
    _announce_cap          = t.announceCap / 100.0f;
    _announce_rate_target  = t.announceRateTarget;
    _announce_rate_grace   = t.announceRateGrace;
    _announce_rate_penalty = t.announceRatePenalty;
  }
};
static void applyAnnounceLimits(RNS::InterfaceImpl& impl) {
  static_cast<LimitsView&>(impl).apply(settings.transport());
}

// Transport hands us verified announces with interface, hops and signal
// stats through the packet-carrying AnnounceHandler callback (our fork /
// upstream PR #85) — no second parse or signature check in the firmware.
class NeighborAnnounceHandler : public RNS::AnnounceHandler {
public:
  void received_announce(const Bytes&, const RNS::Identity&, const Bytes&) override {}
  void received_announce(const Bytes& destHash, const RNS::Identity& identity, const Bytes& appData, const RNS::Packet& packet) override {
    (void)identity;
    if (memcmp(destHash.data(), nodeIdentity.destHash(), Rns::HASH_LEN) == 0) return;
    Neighbor n = {};
    Rns::toHex(destHash.data(), Rns::HASH_LEN, n.hash);
    const uint8_t* d = packet.data().data();               // pub(64) + name_hash(10) + ...
    const char* aspect = packet.data().size() >= 74 ? Rns::aspectName(d + 64) : nullptr;
    strlcpy(n.aspect, aspect ? aspect : "", sizeof(n.aspect));
    Rns::Announce a = {};
    a.appData = appData.data(); a.appDataLen = appData.size();
    Rns::displayName(a, n.name, sizeof(n.name));
    if (aspect && strcmp(aspect, "retimesh.node") == 0) {
      char* sp = strchr(n.name, ' ');
      if (sp) { strlcpy(n.version, sp + 1, sizeof(n.version)); *sp = '\0'; }
    }
    n.kind = NeighborKind::Announce;
    n.hops = packet.hops();
    std::string iface = packet.receiving_interface() ? packet.receiving_interface().name() : std::string();
    n.viaWifi = iface.rfind("LoRa", 0) != 0;
    n.rssi = n.viaWifi ? 0 : packet.rssi();
    n.snr  = n.viaWifi ? 0 : packet.snr();
    neighbors.seen(n);
    g_stats.announcesRx++;
    log_i("announce via %s: %s <%s> \"%s\" hops %u", iface.c_str(), aspect ? aspect : "unknown-aspect", n.hash, n.name, n.hops);
    char line[160];
    snprintf(line, sizeof(line), "announce %s %s <%s> \"%s\" hops=%u rssi=%.0f", iface.c_str(),
             aspect ? aspect : "?", n.hash, n.name, n.hops, (double)n.rssi);
    sdCard.log(line);
  }
};

static RNS::Interface loraIface({RNS::Type::NONE});
struct TcpIface { RNS::Interface handle; TcpClientRnsInterface* impl; };
static std::map<uint32_t, TcpIface> tcpIfaces;

struct Event { bool connect; uint32_t id; char remote[46]; };

// ---------------------------------------------------------------------------
namespace RnsTransport {

bool started() { return sStarted; }

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing) {
  sTxRing = txRing; sRxRing = rxRing; sTcpInRing = tcpInRing;
  sEvents = xQueueCreate(8, sizeof(Event));
  sSnapLock = xSemaphoreCreateMutex();

  if (!settings.transport().enabled) { log_w("Reticulum transport disabled in settings"); return false; }

  try {
    RNS::loglevel(RNS::LOG_INFO);        // DEBUG is compiled in; raise here when tracing

    // Storage: the SD card or the LittleFS partition shared with the web app,
    // as StoreHome decides — it owns that rule, the card's ownership marker
    // and the filesystem the store is pointed at, so this is not the place to
    // re-derive any of it. Decided once, here, because microStore holds files
    // open for the life of the store; moving the store between the two is a
    // deliberate migration that ends in a restart, which is what brings
    // execution back through this line.
    StoreHome::chooseAtBoot();
    rnsFs.init(false);
    log_i("Reticulum storage: %s%s (%u KB free)", RnsFileSystem::prefix(), RNS_FS_ROOT,
          (unsigned)(rnsFs.storageAvailable() / 1024));
    logStoreContents();
    RNS::Utilities::OS::register_filesystem(rnsFs);
    reticulum = RNS::Reticulum();          // ctor resets the storage path, so set it after
    RNS::Reticulum::storagepath(RNS_FS_ROOT);
    reticulum.transport_enabled(true);
    // Housekeeping (announce rebroadcasts, link/receipt timeouts) every
    // second instead of the library default of 60 s (fork / upstream PR #82).
    RNS::Reticulum::jobs_interval(1.0f);
    RNS::Transport::register_announce_handler(std::make_shared<NeighborAnnounceHandler>());

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
  strlcpy(e.remote, remote, sizeof(e.remote));       // IPv4 text, or the IPv6 tail for Auto peers
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
      if (e.id & AutoInterface::AUTO_ID_BASE) {
        const char* tail = strrchr(e.remote, ':');            // last hextet keeps it readable
        snprintf(name, sizeof(name), "Auto/%s", tail ? tail + 1 : e.remote);
      } else {
        snprintf(name, sizeof(name), "WiFi/%s", e.remote);
      }
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
        log_i("deregistered %s", it->second.handle.name().c_str());
        tcpIfaces.erase(it);
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
static size_t   sPathCount = 0;          // full table size; sPaths is capped
static Tables   sTables = {};            // table sizes, for soak monitoring

static void refreshSnapshots() {
  // Walking the path table reads every record back through microStore
  // (LittleFS or the SD card), so refresh it slowly and keep the list short —
  // the count is free, the rows are not.
  if (millis() - sSnapAtMs < SNAPSHOT_INTERVAL_MS) return;
  sSnapAtMs = millis();
  std::vector<PathInfo> p;
  std::vector<IfaceInfo> i;
  double now = RNS::Utilities::OS::time();
  // Paths live in the microStore-backed table (Transport::path_table() is the
  // legacy in-memory container and stays empty in 0.5.x). begin()/end() are
  // non-const, hence the cast off the const accessor.
  auto& pathTable = const_cast<RNS::Persistence::NewPathTable&>(RNS::Transport::new_path_table());
  // A stored path names the interface it was heard on. Interface hashes are
  // derived from the name, so "LoRa" survives a restart but a Wi-Fi client's
  // "WiFi/<ip>" does not — and neither does a peer that has since gone away.
  // Those entries cannot be routed on, so drop them instead of carrying them
  // (and their "Path Interface … not found" warning) forever.
  std::vector<RNS::Bytes> stale;
  for (auto it = pathTable.begin(); it != pathTable.end(); ++it) {
    RNS::Persistence::NewPathTable::Entry& e = *it;
    if (!e.value.receiving_interface()) { stale.push_back(e.key); continue; }
    PathInfo pi = {};
    strlcpy(pi.hash, e.key.toHex().c_str(), sizeof(pi.hash));
    pi.hops = e.value._hops;
    strlcpy(pi.via, e.value.receiving_interface() ? e.value.receiving_interface().name().c_str() : "?", sizeof(pi.via));
    pi.ageS = (uint32_t)max(0.0, now - e.value._timestamp);
    p.push_back(pi);
    if (p.size() >= SNAPSHOT_MAX_PATHS) break;
  }
  if (!stale.empty()) {
    uint16_t dropped = RNS::Transport::remove_paths(stale);
    log_i("dropped %u stored path(s) whose interface is gone", (unsigned)dropped);
  }
  sPathCount = pathTable.size();
  for (const auto& iface : RNS::Transport::get_interfaces()) {
    IfaceInfo ii = {};
    strlcpy(ii.name, iface.name().c_str(), sizeof(ii.name));
    strlcpy(ii.mode, modeNameOf(iface.mode()), sizeof(ii.mode));
    ii.rxb = iface.rxbytes(); ii.txb = iface.txbytes();
    i.push_back(ii);
  }
  // Sizes only — every one of these is a container the RNS task owns, so they
  // are read here and published under the same lock as the rest, never touched
  // from the web task.
  Tables t = {};
  t.paths         = (uint32_t)sPathCount;
  t.links         = (uint32_t)RNS::Transport::link_table().size();
  t.activeLinks   = (uint32_t)RNS::Transport::active_links().size();
  t.pendingLinks  = (uint32_t)RNS::Transport::pending_links().size();
  t.destinations  = (uint32_t)RNS::Transport::destinations().size();
  t.announces     = (uint32_t)RNS::Transport::announce_table().size();
  t.heldAnnounces = (uint32_t)RNS::Transport::held_announces().size();
  t.rates         = (uint32_t)RNS::Transport::announce_rate_table().size();

  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  sPaths.swap(p); sIfaces.swap(i);
  sTables = t;
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
  size_t n = sPathCount;                 // whole table, even when the list is capped
  xSemaphoreGive(sSnapLock);
  return n;
}

Tables tables() {
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  Tables t = sTables;
  xSemaphoreGive(sSnapLock);
  return t;
}

void loop() {
  if (!sStarted) { vTaskDelay(pdMS_TO_TICKS(500)); return; }
  try {
    processEvents();
    drainTcp();
    reticulum.loop();                      // interface loops + housekeeping (jobs_interval = 1 s)

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
