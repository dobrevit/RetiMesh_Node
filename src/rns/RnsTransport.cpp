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
#include <atomic>
#include <map>
#include <vector>
#include "RnsFileSystem.h"
#include "Neighbors.h"
#include "SdCard.h"
#include "RnsAnnounce.h"
#include "LxmfFormat.h"
#include "LxmfInbox.h"
#include "Buzzer.h"
#include "LxmfCommands.h"
#include "Telemetry.h"
#include "NomadNet.h"
#include "Diag.h"
#include "RingItem.h"
#include "Lock.h"
#include "Power.h"
#include "Gps.h"
#include <LittleFS.h>
#include "RnsAdmin.h"
#include <esp_random.h>
#include "Lock.h"
#include "Settings.h"
#include "LoRaRadio.h"
#include "RetiTransportServer.h"
#include "WifiManager.h"
#include "AutoInterface.h"

using RNS::Bytes;

static RNS::Reticulum   reticulum({RNS::Type::NONE});
static RNS::Identity    nodeRnsIdentity({RNS::Type::NONE});
static RNS::Destination nodeDest({RNS::Type::NONE});
// The node's LXMF address, under the same identity as retimesh.node. It is a
// second destination rather than a second aspect on the first: the
// destination hash is over the aspects, so lxmf.delivery is a different
// address that LXMF clients already look for. Without it a RetiMesh node is
// invisible to them — Sideband lists LXMF and NomadNet aspects and hides
// everything else, so its announce stream stays empty however many nodes are
// in earshot (docs/troubleshooting.md).
static RNS::Destination lxmfDest({RNS::Type::NONE});
// And the node as something to *read* rather than message. NomadNet browses
// over Reticulum, so a page answers over whatever carried the request — the
// LoRa channel included, which is the one surface the HTTP portal can never
// reach (NomadNet.h).
static RNS::Destination nomadDest({RNS::Type::NONE});

// The last message this node was sent, and how many have arrived. Not an
// inbox — a node that announces an LXMF address and then silently drops what
// is sent to it is worse than one that never advertised, and this is the
// smallest thing that makes it visible. A real store belongs with the
// propagation-node work.
static std::atomic<uint32_t> sLxmfRx{0}, sLxmfRejected{0}, sLxmfUnverified{0}, sLxmfMismatched{0};
// The largest transfer this node will assemble in RAM for a delivery. Every
// administrative command and every written message is far below it; a claim
// above it is refused rather than believed (onLxmfResourceStarted).
static constexpr size_t kLxmfResourceMax = 8 * 1024;
static RnsFileSystem    rnsFs;
static bool             sStarted = false;

static RingbufHandle_t  sTxRing = nullptr, sRxRing = nullptr, sTcpInRing = nullptr;
static QueueHandle_t    sEvents = nullptr;
static SemaphoreHandle_t sSnapLock = nullptr;
static uint32_t         sNextAnnounceMs = 0;
static uint32_t         sAnnounceFloorMs = 0;   // nothing announces before this

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

// The one mode that stops announces reaching a neighbour outright, asked of
// toMode() rather than compared against a number, so the rule stays in one
// place if the vocabulary ever grows.
static bool blocksAnnounces(uint8_t m) {
  return toMode(m) == RNS::Type::Interface::MODE_ACCESS_POINT;
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
    refreshBitrate();
  }
  // RNS's own formula (RNodeInterface): the channel's payload bitrate. It
  // decides the announce cap's airtime budget, so it has to follow the
  // channel — which the settings page applies live. Computed once in the
  // constructor, it went on describing the channel the node booted on.
  void refreshBitrate() {
    const RadioSettings& r = settings.radio();
    if (r.sf == _sf && r.cr == _cr && r.bwKhz == _bw) return;
    _sf = r.sf; _cr = r.cr; _bw = r.bwKhz;
    _bitrate = (uint32_t)(r.sf * ((4.0 / r.cr) / (pow(2.0, r.sf) / r.bwKhz)) * 1000.0);
  }
  bool send_outgoing(const Bytes& data) override {
    if (!g_stats.radioOnline) return false;
    if (xRingbufferSend(sTxRing, data.data(), data.size(), pdMS_TO_TICKS(20)) != pdTRUE) {
      log_w("LoRa TX ring full, dropping %u bytes", (unsigned)data.size());
      return false;
    }
    // What the interface has sent, for /api/status and the transport page.
    // Handed to the ring rather than confirmed on the air: that is the
    // boundary this interface owns, and the radio's own counters
    // (g_stats.loraTxPackets) are what say whether it left.
    handle_outgoing(data);
    return true;
  }
  void loop() override {
    refreshBitrate();
    size_t sz = 0;
    uint8_t* item;
    while ((item = (uint8_t*)xRingbufferReceive(sRxRing, &sz, 0)) != nullptr) {
      // Given back however this body is left, including by a throw out of
      // handle_incoming below (Sys::RingItem). Returned by hand, a bad_alloc
      // there leaked a frame's worth of ring on every failure and the node
      // eventually went deaf with every counter still rising.
      Sys::RingItem held(sRxRing, item);
      // The reading the radio took of this frame, carried with it rather than
      // sampled now: draining three at once used to stamp all three with the
      // newest one's RSSI (Config.h, LoRaRxFrame).
      if (sz <= sizeof(LoRaRxFrame)) {
        // A header with no frame behind it. Cannot happen from the radio task,
        // which only posts what it decoded — but the producer has already
        // counted it, so dropping it silently would leave the two ends of the
        // ring disagreeing with nothing to say so.
        g_stats.loraRxBadLength++;
        continue;
      }
      {
        LoRaRxFrame hdr;
        memcpy(&hdr, item, sizeof(hdr));
        // Signal stats are private to InterfaceImpl; the wrapper exposes setters.
        std::shared_ptr<RNS::InterfaceImpl> sp = shared_from_this();
        RNS::Interface self(sp);
        self.r_stat_rssi(hdr.rssi);
        self.r_stat_snr(hdr.snr);
        // Quality is not a figure any radio reports; RNS derives it, and so
        // does the panel. Nothing set it before, so the line a signal report
        // promised could never appear.
        self.r_stat_q((float)loraQualityPercent(hdr.snr, settings.radio().sf));
        handle_incoming(Bytes(item + sizeof(hdr), sz - sizeof(hdr)));
      }
    }
  }
private:
  uint8_t _sf = 0, _cr = 0;
  float   _bw = 0;
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
    const bool sent = (_id & AutoInterface::AUTO_ID_BASE)
                    ? AutoInterface::sendTo(_id, data.data(), data.size())
                    : transportServer.sendTo(_id, data.data(), data.size());
    // Only what actually went: a peer whose socket has gone, or an
    // AutoInterface datagram that could not be posted, is a send that did not
    // happen and must not read as one.
    if (sent) handle_outgoing(data);
    return sent;
  }
  void incoming(const uint8_t* p, size_t len) {
    handle_incoming(Bytes(p, len));
  }
private:
  uint32_t _id;
};

// The limits live in protected InterfaceImpl fields, and the public Interface
// wrapper exposes setters for three of the four. Reaching the fourth goes
// through pointers-to-member named in a derived class: naming a protected base
// member through a derived type is what makes it accessible, and the pointer
// that comes back is a pointer to the *base* member, so it applies to any
// interface implementation. This used to static_cast an InterfaceImpl& to
// LimitsView& — a cast to a sibling type the object never was, which is
// undefined behaviour whatever it does in practice.
struct LimitsView : RNS::InterfaceImpl {
  static void apply(RNS::InterfaceImpl& impl, const TransportSettings& t) {
    impl.*(&LimitsView::_announce_cap)          = t.announceCap / 100.0f;
    impl.*(&LimitsView::_announce_rate_target)  = t.announceRateTarget;
    impl.*(&LimitsView::_announce_rate_grace)   = t.announceRateGrace;
    impl.*(&LimitsView::_announce_rate_penalty) = t.announceRatePenalty;
  }
};
static void applyAnnounceLimits(RNS::InterfaceImpl& impl) {
  LimitsView::apply(impl, settings.transport());
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

// An AutoInterface peer is another node on the LAN or on our access point; a
// plain id is a client on :4242. They get their modes from different settings
// because they are different kinds of neighbour — see TransportSettings.
static bool isAutoPeer(uint32_t id) { return (id & AutoInterface::AUTO_ID_BASE) != 0; }

static RNS::Type::Interface::modes modeFor(uint32_t id) {
  const TransportSettings& t = settings.transport();
  return toMode(isAutoPeer(id) ? t.autoMode : t.wifiMode);
}

// Every interface Transport holds is identified by the hash of its name
// (RNS::Interface::get_hash), so a name has to be unique per registration or
// register_interface() silently keeps the interface already under that hash
// and drops the new one — leaving a peer that can send to us and never hears
// a thing back. RNS's own TCPServerInterface names a spawned interface after
// the remote address *and port* for exactly this reason: the port is what
// makes a phone reconnecting from the same address a different interface.
// An Auto peer carries its whole link-local address rather than one hextet of
// it, because the address is the peer's identity — a hextet is 16 bits of an
// EUI-64 that two boards from one production run may well share. A link-local
// compresses to at most "fe80::a:b:c:d", so both forms fit a name.
static void interfaceName(uint32_t id, const char* remote, char* out, size_t cap) {
  snprintf(out, cap, "%s/%s", isAutoPeer(id) ? "Auto" : "WiFi", remote);
}

// ---------------------------------------------------------------------------
namespace RnsTransport {

// Published under sSnapLock, plus a staging copy each so a refresh can be
// built without holding the lock and then handed over with a swap. Both halves
// are reserved to their maximum once, at begin(), and swapping exchanges two
// already-reserved buffers -- so after startup neither ever allocates again.
static std::vector<PathInfo>  sPaths,  sPathsStaging;
static std::vector<IfaceInfo> sIfaces, sIfacesStaging;

bool started() { return sStarted; }

// The library's level while it may print, and whether it may. The mute is
// asked for from another task and applied on this one, in loop().
static constexpr RNS::LogLevel kLogLevel = RNS::LOG_INFO;
static std::atomic<bool> sMuteWanted{false};
static bool sMuted = false;

void muteLog(bool mute) { sMuteWanted = mute; }

static void applyLogMute() {
  const bool want = sMuteWanted;
  if (want == sMuted) return;
  RNS::loglevel(want ? RNS::LOG_NONE : kLogLevel);
  sMuted = want;
}

// An LXMF message arrived. The destination has already decrypted it, so what
// is left is the envelope (RnsAnnounce.h) and the question of whether to
// believe it.
//
// Believing it means checking the signature against the sender's public key,
// and the only way to have that key is to have heard the sender announce. A
// sender this node has not heard cannot be checked at all, which is not the
// same as one that failed the check — the three outcomes are separated below,
// and the standing each one earns is what a privileged action will later ask
// about.
//
// A message that is taken is proved, which is what makes this different from
// announcing a delivery address and dropping what arrives: the sender's
// client shows the message delivered, because it was. Returns whether it was
// taken, which is what the callers prove on — a proof is this node saying
// "that arrived", and saying it about bytes that were not a message at all
// would be a lie the sender's client believes.
// A key a sender proved on its own link, held in RAM for this run only.
//
// Deliberately not Identity::remember(). microReticulum stores a known
// destination only when it does not already hold one — a documented
// divergence in Identity.cpp, made to lessen flash wear — so an entry written
// from here would be that peer's entry for good: its own announces would then
// take the "already known" branch and be skipped, leaving its app_data (which
// is where its name lives) permanently empty and its timestamp frozen at this
// moment, so it is the first evicted when the table is full. Remembering a
// key from a link would have made the node worse at the thing announces are
// for. The store is for what announces teach; this is for what a link proves.
//
// Eight is more correspondents than a node in earshot of a village has at
// once, and costs 400 bytes rather than a flash write per sender. Oldest goes
// first, and losing one costs nothing: the sender identifies again on its
// next link. Touched only from the RNS task — the identify callback and
// message handling both run inside reticulum.loop() — so no lock.
// A whole public key, which for Reticulum is two: the X25519 half used to
// encrypt to a peer and the Ed25519 half used to check what it signed.
// get_public_key() hands back both halves concatenated, so 32 bytes is exactly
// half a key. Taken from the library rather than written out, so that a node
// and the stack it runs on cannot disagree about how long a key is.
static const size_t kPublicKeyLen = RNS::Type::Identity::KEYSIZE / 8;
struct ProvenKey { uint8_t hash[16]; uint8_t key[kPublicKeyLen]; uint32_t atMs; bool used; };
static ProvenKey sProven[8];

static const ProvenKey* provenKeyFor(const uint8_t* sourceHash) {
  for (const auto& e : sProven)
    if (e.used && memcmp(e.hash, sourceHash, 16) == 0) return &e;
  return nullptr;
}

static void rememberProvenKey(const Bytes& source, const Bytes& key) {
  // 32 here rejected every key there is. get_public_key() returns both halves,
  // so nothing was ever remembered: the table stayed empty, provenKeyFor()
  // always said no, and a sender who identified on a link but had never
  // announced was counted unverified and answered with silence — the one case
  // this table exists for.
  if (source.size() != 16 || key.size() != kPublicKeyLen) return;
  ProvenKey* slot = nullptr;
  for (auto& e : sProven) {
    if (e.used && memcmp(e.hash, source.data(), 16) == 0) { slot = &e; break; }
    if (!e.used && !slot) slot = &e;
  }
  if (!slot) {                                   // all in use: the oldest goes
    slot = &sProven[0];
    for (auto& e : sProven) if ((int32_t)(e.atMs - slot->atMs) < 0) slot = &e;
  }
  memcpy(slot->hash, source.data(), 16);
  memcpy(slot->key, key.data(), kPublicKeyLen);
  slot->atMs = millis();
  slot->used = true;
}

// What the radio measured of the packet that brought a message. microReticulum
// copies the interface's reading onto every packet it takes in
// (Transport.cpp), and our LoRa interface sets it from the last frame the
// radio decoded — so this is a real measurement on the RF path and NaN
// everywhere else, which is what a signal report should say.
static Rns::Commands::Signal signalOf(const RNS::Packet& packet) {
  Rns::Commands::Signal s;
  s.rssi = packet.rssi();
  s.snr  = packet.snr();
  s.q    = packet.q();
  return s;
}

// One peer cannot have the radio to itself. The reply queue is a single slot
// drained once per pass, so throughput is already bounded; this stops one
// sender filling every slot there is. Ten seconds is far below what a person
// pressing a button does and far above what a flood needs to be stopped.
//
// Same table shape and the same reasoning as the proven keys above: eight
// entries, oldest out, RNS task only, no lock.
static const uint32_t kCommandCooldownMs = 10000;
struct CommandSeen { uint8_t hash[16]; uint32_t atMs; bool used; };
static CommandSeen sCommandSeen[8];

// Asking and recording are separate on purpose. Recording inside the question
// spent a sender's cooldown on a command this node does not answer, so a
// telemetry request — which Sideband sends and this node cannot answer yet —
// made the ping after it look like the node had gone quiet.
static bool commandCooldownActive(const uint8_t* sourceHash) {
  for (const auto& e : sCommandSeen)
    if (e.used && memcmp(e.hash, sourceHash, 16) == 0)
      return (uint32_t)(millis() - e.atMs) < kCommandCooldownMs;
  return false;
}

// What every sender gets between them.
//
// The table above holds eight hashes and evicts the oldest, so it gates a
// sender who keeps one identity and nothing else: announce nine throwaway
// identities and each request evicts the entry that would have stopped the
// next one, so the gate never fires. Announcing is free and being `verified`
// asks for nothing more, so the cost of defeating a per-sender rule is the
// cost of generating keys.
//
// What that bought an attacker was the node's transmit budget. Every answer is
// a signed LXMF packet; enough of them and refreshAirtimeStats() sets
// dutyLocked, LoRaRadio stops draining the TX ring, and the node's own
// transport traffic and admin answers stop with it — for the rest of the hour.
//
// So there is a second rule that no identity can be rotated out of: a bucket
// of four answers, refilling one every fifteen seconds. A person pressing a
// button gets an immediate answer and three more; a flood gets four per
// minute however many names it wears.
static const uint32_t kCommandBurst    = 4;
static const uint32_t kCommandRefillMs = 15000;
static uint32_t sCommandTokens   = kCommandBurst;
static uint32_t sCommandFilledMs = 0;

static void refillCommandBudget() {
  const uint32_t now = millis();
  const uint32_t due = (uint32_t)(now - sCommandFilledMs) / kCommandRefillMs;
  if (!due) return;
  sCommandTokens = (sCommandTokens + due > kCommandBurst) ? kCommandBurst : sCommandTokens + due;
  sCommandFilledMs += due * kCommandRefillMs;         // keep the remainder, so it does not drift
}

// The one question both rules live behind, so a caller cannot ask half of it.
static bool mayAnswerCommand(const uint8_t* sourceHash) {
  refillCommandBudget();
  return sCommandTokens > 0 && !commandCooldownActive(sourceHash);
}

static void noteCommandAnswered(const uint8_t* sourceHash) {
  if (sCommandTokens) sCommandTokens--;
  CommandSeen* slot = nullptr;
  for (auto& e : sCommandSeen) {
    if (e.used && memcmp(e.hash, sourceHash, 16) == 0) { slot = &e; break; }
    if (!e.used && !slot) slot = &e;
  }
  if (!slot) {
    slot = &sCommandSeen[0];
    for (auto& e : sCommandSeen) if ((int32_t)(e.atMs - slot->atMs) < 0) slot = &e;
  }
  memcpy(slot->hash, sourceHash, 16);
  slot->atMs = millis();
  slot->used = true;
}

// The key this node holds for a source hash, wherever it came from: an announce
// it heard, or a link on which the sender proved who they are. Asked in one
// place because the receiving side and the answering side must not disagree
// about whether a sender is known — they did, and a signal report from someone
// who had identified on a link but never announced was verified on the way in
// and then dropped on the way out with "no key for ...", which is the one user
// the feature is justified by.
static RNS::Identity senderKey(const uint8_t sourceHash[16]) {
  RNS::Identity known = RNS::Identity::recall(RNS::Bytes(sourceHash, 16));
  if (known) return known;
  if (const ProvenKey* proven = provenKeyFor(sourceHash)) {
    RNS::Identity fromLink(false);
    // Both halves. Given 32, load_public_key() left _sig_pub_bytes empty, so
    // validate() had nothing to check a signature with and the identity hash
    // was derived from half the key — addressing a reply to a destination that
    // is not the sender's.
    fromLink.load_public_key(RNS::Bytes(proven->key, kPublicKeyLen));
    return fromLink;
  }
  return {RNS::Type::NONE};
}

static bool handleLxmfMessage(const RNS::Bytes& data, uint8_t via,
                              const Rns::Commands::Signal& signal) {
  const char* how = Rns::viaName(via);
  Rns::LxmfMessage m;
  if (!Rns::parseLxmf(data.data(), data.size(), m)) {
    sLxmfRejected++;
    log_w("lxmf: a message arrived that is not one; ignored");
    return false;
  }
  // Three outcomes, not two, and the difference matters.
  //
  // A sender this node has heard announce, whose signature checks out, is
  // *verified*: the message is theirs and nobody else's. That is the standing
  // this node will require before it lets anything drive a privileged action.
  //
  // A sender it has heard, whose signature does not check out, is a forgery
  // attempt, and is the one case worth refusing outright.
  //
  // A sender it has never heard announce cannot be verified at all — and
  // refusing those was wrong. The message decrypted with this node's private
  // key, so it was genuinely addressed here; what is missing is proof of who
  // sent it, which is not the same as evidence that anyone lied. Most senders
  // will not have announced to this node lately, and a node that has just
  // rebooted has heard nobody at all. Refusing them made a node that
  // advertises a delivery address and silently drops what arrives, which is
  // exactly what announcing one was meant to stop. They are taken, marked
  // unverified, and never trusted with more than being read.
  const RNS::Bytes source(m.sourceHash, 16);
  RNS::Identity sender = senderKey(m.sourceHash);
  bool verified = false;
  if (sender) {
    // What LXMF actually signs is the two hashes and the payload *and then
    // the hash of those three* — hashed_part followed by full_hash(hashed_
    // part). Signing only the first three verifies nothing a real client
    // sent: every message from a sender this node knew was refused as a
    // forgery, which is a worse failure than not checking at all, because it
    // accuses the honest sender.
    //
    // The signature still binds the message to this pair of addresses, so the
    // same bytes replayed at another node do not verify and neither does an
    // altered text; the appended hash is LXMF's own, and matching it is what
    // makes this interoperable rather than merely self-consistent.
    // The order comes from LxmfFormat.h rather than being repeated here, so
    // that this and the vector tests cannot come to disagree about it — a test
    // that rebuilt the sequence for itself would stay green through a change
    // to this line, which is the one thing it is there to catch.
    RNS::Bytes hashed_part;
    Rns::lxmfSignedSpans(m, [&](const uint8_t* p, size_t n) { hashed_part.append(p, n); });
    RNS::Bytes signed_data(hashed_part);
    signed_data.append(RNS::Identity::full_hash(hashed_part));
    verified = sender.validate(RNS::Bytes(m.signature, 64), signed_data);
    if (!verified) {
      // Still taken rather than refused, and now for a settled reason rather
      // than an open question. The mismatch that used to be unexplained was
      // the stamp: LXMF signs the four-element payload as it stood before one
      // was appended, so the same text from the same sender verified or did
      // not depending on whether a stamp came with it. That is understood and
      // handled where the signed bytes are worked out (LxmfFormat.h).
      //
      // What is left is a genuine failure to match, and it is shown rather
      // than dropped because dropping it tells the operator nothing: a
      // message whose sender cannot be proved is worth reading and worth
      // marking. Nothing privileged follows from it — remote administration
      // requires `verified` and refuses these outright, which is the refusal
      // this comment used to promise (RnsAdmin.h).
      sLxmfMismatched++;
      log_w("lxmf: message from %s does not match the key this node holds for it. Taken as "
            "unverified rather than refused: this node's checking is not above suspicion "
            "(RnsTransport.cpp). It will never be trusted with anything privileged.",
            source.toHex().c_str());
    }
  }

  sLxmfRx++;
  // The content element is not guaranteed to be a string: a payload of
  // [ts, "", nil, {}] parses perfectly well and leaves content null with a
  // length of zero, and anyone can send one, since unverified messages are
  // taken by design. memcpy and %.*s are both undefined on a null pointer
  // even for zero bytes, and a compiler is entitled to reason from that.
  const uint8_t* text = m.content ? m.content : (const uint8_t*)"";
  // Cut on a character boundary. A body truncated at a fixed byte count lands
  // inside a multi-byte sequence for most non-Latin text, and what is stored
  // then is not UTF-8 — which goes on to the messages page as JSON.
  const size_t textLen = Rns::utf8TrimLen(text, m.contentLen, m.contentLen);
  // "Unverified" is the sender this node has never heard announce — there was
  // no key to check against. A sender it has heard whose signature does not
  // match is a different thing entirely, already counted as mismatched just
  // above, and counting it here as well made the two indistinguishable in the
  // one place an operator would look to tell them apart.
  if (!sender) sLxmfUnverified++;
  // Queued for the loop task, not written here. The standing is recorded
  // rather than recomputed later: whether this node could check a sender is a
  // fact about the moment the message arrived, and an identity heard five
  // minutes afterwards does not make an old message verified.
  //
  // The signature identifies the message, which is how a retransmission — what
  // a sender does when it does not see a proof, and over LoRa that is ordinary
  // — is recognised as the message it already has rather than taking a second
  // of the fifty slots.
  //
  // The text handed over is trimmed to a character boundary at the record's
  // own limit, rather than left for the record to cut: the store truncates at
  // a byte count, and a body cut there is not UTF-8 by the time the messages
  // page tries to encode it as JSON.
  uint64_t sig = 0;
  for (int i = 0; i < 8; i++) sig = (sig << 8) | m.signature[i];
  const uint8_t standing = verified ? Rns::StandingVerified
                         : !sender  ? Rns::StandingNoKey
                                    : Rns::StandingMismatch;
  if (!Rns::Inbox::note(m.sourceHash, standing,
                        via, m.sentAt, (const char*)text,
                        Rns::utf8TrimLen(text, textLen, Rns::kInboxTextMax), sig))
    log_d("lxmf: not stored (a repeat, or arriving faster than the store is written)");
  else
    Buzzer::message();                   // carried devices get told out loud
  // The same three standings the counters keep, said in words. This used to
  // read off `verified` alone, so a sender whose signature did not match was
  // reported as one this node had never heard announce — which is the one
  // thing the log could say that points an operator away from what happened.
  //
  // The body goes through the console, which is a terminal, and it arrives
  // from whoever is in earshot. Printed raw, an escape sequence in a message
  // clears the operator's screen or hides the lines around it — so what is
  // shown is the text with anything not printable replaced, cut on a
  // character boundary.
  char shown[81];
  Rns::utf8SafeCopy(text, textLen, shown, sizeof(shown));
  log_i("lxmf: %s%s message from %s over %s (%u bytes): %s",
        verified   ? "verified"
        : !sender  ? "UNVERIFIED (this node has not heard that sender announce)"
                   : "UNVERIFIED (the signature does not match the key this node holds)",
        // A stamp only arrives when this node announced a cost, which it no
        // longer does — so seeing one beside a mismatch says the announce is
        // wrong rather than the sender, and that is the single most useful
        // thing the log can say about a mismatch.
        m.stamped ? ", carrying a stamp" : "",
        source.toHex().c_str(), how, (unsigned)m.contentLen, shown);
  // Most of what LXMF carries is not the text: an image, a file, a telemetry
  // reading and a command all travel in the fields map, and a message that is
  // only those arrives with nothing to read. Saying so is the difference
  // between "somebody sent an empty message" and "somebody sent a photo this
  // node does not open".
  //
  // The stored record does not carry this yet — it is a fixed two hundred
  // bytes with no room left (LxmfInbox.h), and changing that layout is a
  // decision for whoever owns the format.
  if (m.fieldsCount)
    log_i("lxmf: ...and %u field%s this node does not read, %u bytes (attachment, telemetry "
          "or a command)", (unsigned)m.fieldsCount, m.fieldsCount == 1 ? "" : "s",
          (unsigned)m.fieldsLen);
  // Last, and only after the message has been recorded and said out loud: a
  // command that turns out to be one is still a message, and a node that ran
  // it without keeping a copy would have no account of what it was told to do.
  // The gate refuses everything by default and says which of its questions
  // failed (RnsAdmin.h).
  Rns::Admin::offer(m.sourceHash, standing, m.sentAt, (const char*)text, textLen);

  // And the questions that are not administration: is this node there, say
  // this back, how well did it hear me (LxmfCommands.h).
  //
  // Only for a verified sender, and the reason is not the one the admin gate
  // has. It is that a source hash on an unverified message is a *claim* — the
  // sender wrote it into the payload and nothing checked it — so a reply goes
  // to whoever the claim named rather than to whoever sent it. Answering one
  // would have made this node a way to put attacker-chosen text, over its own
  // signature, into a stranger's conversation, and to aim its transmitter at a
  // third party. RnsAdmin refuses to answer an unverified hash for exactly
  // this reason (RnsAdmin.cpp); the rule is the node's, not that feature's.
  //
  // It also gives the cooldown below something real to key on: a rate limit on
  // a hash anybody can fabricate limits nobody.
  //
  // The person at the edge of coverage is still served, which was the worry:
  // a sender who never announced but identified on its link is verified from
  // that (senderKey), which is the case a signal report is most wanted in.
  if (settings.maintenance().lxmfCommands && verified && m.fieldsCount) {
    const uint8_t* val = nullptr; size_t valLen = 0;
    if (Rns::lxmfField(m.fields, m.fieldsLen, Rns::kFieldCommands, val, valLen)) {
      Rns::LxmfCommand cmds[4];
      const size_t n = Rns::lxmfCommands(val, valLen, cmds, 4);
      if (n && !mayAnswerCommand(m.sourceHash)) {
        log_d("lxmf: not answering %s (%u answer(s) left in the budget)",
              source.toHex().c_str(), (unsigned)sCommandTokens);
      } else {
        // One answer per message, whatever was asked. The reply queue is two
        // deep and shared with remote administration, so a message carrying
        // three commands used to queue three replies, drop the third, and be
        // able to push an admin answer out of the way. It also keeps the claim
        // this feature is defended by true: one short reply for one short
        // request, never four.
        for (size_t k = 0; k < n; k++) {
          // Which commands are answered, and how, is LxmfCommands.h's to say —
          // both this and reply() ask it, so they cannot come to disagree
          // about whether telemetry is answered.
          const Rns::Commands::Answer kind = Rns::Commands::answers(cmds[k].id);
          if (kind == Rns::Commands::Answer::None) continue;
          const bool wantsTelemetry = kind == Rns::Commands::Answer::Telemetry;
          char answer[Rns::Commands::kReplyMax] = "";
          // A telemetry answer carries readings and no text: a sentence beside
          // them would appear in the conversation as a message somebody sent.
          if (!wantsTelemetry &&
              !Rns::Commands::reply(cmds[k], signal, answer, sizeof(answer))) continue;
          // The key this message was checked against, so the answer does not
          // ask who the sender is a second time and cannot get a different
          // answer than the check did.
          const RNS::Bytes senderPub = sender.get_public_key();
          const uint8_t* verifiedKey =
              senderPub.size() == kPublicKeyLen ? senderPub.data() : nullptr;
          if (queueLxmfTelemetry(m.sourceHash, answer, wantsTelemetry, signal, verifiedKey)) {
            // Stamped only now. Stamping before knowing an answer exists spent
            // the sender's ten seconds on a command this node does not answer,
            // so a telemetry request made the ping after it look like silence.
            noteCommandAnswered(m.sourceHash);
            log_i("lxmf: answering command 0x%02X from %s", (unsigned)cmds[k].id,
                  source.toHex().c_str());
          } else {
            log_w("lxmf: could not queue an answer for %s", source.toHex().c_str());
          }
          break;
        }
      }
    }
  }
  return true;
}

// A page costs one packet and one proof; two seconds between them is far
// below what a person reading costs and far above what a flood needs to be
// stopped.
static const uint32_t kPageCooldownMs = 2000;

// The node's own page, generated when it is asked for.
//
// The request generator hands back the msgpack-encoded value, not the text:
// microReticulum appends it after the response envelope's array header, so
// what goes on the wire has to be the encoded form. Bytes, because NomadNet
// reads a page with .decode("utf-8") — the same str-against-bin distinction
// that made this node nameless for a release, answered the other way here.
static RNS::Bytes serveIndex(const RNS::Bytes& path, const RNS::Bytes& data,
                             const RNS::Bytes& request_id, const RNS::Bytes& link_id,
                             const RNS::Identity& remote_identity, double requested_at) {
  (void)path; (void)data; (void)request_id; (void)link_id;
  (void)remote_identity; (void)requested_at;

  Rns::NomadNet::Status st;
  const RadioSettings& r = settings.radio();
  st.name = loraRadio.callsign();
  st.version = FW_VERSION;
  st.board = BOARD_NAME;
  const std::string self = nomadDest ? nomadDest.hash().toHex() : std::string();
  const std::string lxmf = lxmfDest ? lxmfDest.hash().toHex() : std::string();
  st.address = self.c_str();
  st.lxmfAddress = lxmf.c_str();
  st.uptimeS = (uint32_t)(millis() / 1000);

  st.radioOnline = g_stats.radioOnline;
  st.radioModel = g_stats.radioModel;
  st.freqMhz = r.freqMhz; st.bwKhz = r.bwKhz; st.sf = r.sf; st.cr = r.cr; st.txDbm = r.txDbm;
  // Any reception at all, not only a reassembled RNS packet: lastRssi is
  // written on every CRC-good frame, so a node sitting beside a beaconing
  // neighbour holds a real reading while loraRxPackets is still zero — and
  // would have printed "nothing heard", which is the falsehood this branch
  // exists to avoid.
  st.heardAnything = g_stats.loraRxPackets || g_stats.beaconsRx || g_stats.loraRxDropRing;
  st.channelRefused = g_stats.radioApplyError != 0;
  st.lastRssi = g_stats.lastRssi; st.lastSnr = g_stats.lastSnr;
  st.rxPackets = g_stats.loraRxPackets; st.txPackets = g_stats.loraTxPackets;

  const Tables t = tables();
  st.interfaces = (uint32_t)interfaceCount();
  st.paths = t.paths;
  st.links = t.activeLinks;

  st.lxmfRx = sLxmfRx; st.lxmfUnverified = sLxmfUnverified; st.lxmfMismatched = sLxmfMismatched;

  const Power::Battery b = Power::battery();
  st.haveBattery = b.present;
  st.batteryPct = b.percent;
  st.charging = b.charging;
  st.chargeKnown = b.chargeKnown;
  // Asked of Diag rather than measured again here. Which heap figure is the
  // honest one — byte-addressable internal RAM, not the total including PSRAM
  // — is a rule with a reason, and it was written out in three places; two of
  // them could drift.
  const Diag::Heap heap = Diag::heap();
  st.heapFree = heap.freeDram;
  st.heapLargest = heap.largestDramBlock;

  // Answering costs airtime on a duty-limited radio, and anyone who can route
  // here can ask. The commands next door hold a per-sender cooldown for the
  // same reason; a page has no sender to key on — a request carries an
  // identity only if the client chose to identify — so the limit is on the
  // node: one page at a time, and not again within the cooldown. A reader
  // refreshing gets the page they already have from their own client, which
  // is why it also carries a no-cache header (NomadNet.h).
  static uint32_t sLastPageMs = 0;
  if (sLastPageMs && (uint32_t)(millis() - sLastPageMs) < kPageCooldownMs) {
    log_d("nomadnet: not serving another page so soon");
    return {RNS::Bytes::NONE};
  }
  sLastPageMs = millis();

  // Bounded on purpose: this runs on the RNS task because a stranger asked,
  // and the buffer is what says how much of that task's stack a request can
  // spend. A page longer than this is cut with a line saying so.
  //
  // One buffer, not two. The msgpack header is written into three bytes
  // reserved ahead of the text rather than the whole page being copied into a
  // second buffer to be encoded — which halves what a request costs the stack
  // of the task that also drives the radio and the whole Reticulum loop.
  // Sized to what leaves in one packet (NomadNet.h), so the answer is one
  // transmission and one proof rather than a resource transfer.
  uint8_t out[3 + Rns::NomadNet::kPageMax];
  const size_t n = Rns::NomadNet::index(st, (char*)out + 3, sizeof(out) - 3);
  if (!n) return {RNS::Bytes::NONE};
  out[0] = 0xC5;                                  // bin16: fixed width, so the text never moves
  out[1] = (uint8_t)(n >> 8);
  out[2] = (uint8_t)n;
  log_i("nomadnet: served the index page (%u bytes)", (unsigned)n);
  return RNS::Bytes(out, 3 + n);
}

// A message can reach a delivery address two ways, and a node that handles
// only one of them looks broken to a client that chose the other.
//
//   opportunistic   one packet straight to the destination. Cheap, and what a
//                   client uses for something short when it already has a path.
//   direct          over a Link the client establishes first. What clients
//                   prefer for anything else, because the link carries proofs
//                   and can be reused.
//
// The link is the one that was missing: microReticulum establishes an inbound
// link whether or not anyone is listening for it, and calls the destination's
// link-established callback only if there is one. Without this the link came
// up, the client sent its message into it, and the node decrypted it and
// dropped it on the floor — every message, silently, while the announce it
// answered looked perfectly healthy.
// Tell the sender it arrived. Without this the message is delivered and their
// client says otherwise, which is the same silence as dropping it.
static void proveIfTaken(const RNS::Packet& packet, bool taken) {
  if (taken) const_cast<RNS::Packet&>(packet).prove();
}

static void onLxmfLinkPacket(const RNS::Bytes& data, const RNS::Packet& packet) {
  proveIfTaken(packet, handleLxmfMessage(data, Rns::ViaLink, signalOf(packet)));
}

// A message too big for one link packet is not sent as several: the client
// packs it as a Resource and advertises that instead. A link's resource
// strategy defaults to ACCEPT_NONE, so those advertisements were dropped
// where the link handles them (Link.cpp) and nothing downstream ever saw the
// message — the same silent floor the missing link callback used to be, and
// hit by exactly the messages people write rather than the short ones a test
// sends.
//
// This does not make every long message arrive, and it would be dishonest to
// leave it looking as though it did. microReticulum has no bz2 and refuses an
// inbound resource that is flagged compressed (Resource.cpp), while RNS
// compresses a resource whenever that makes it smaller — which for text is
// always. So a long message from a real client is still refused, now by the
// library rather than by this file. What changes here is worth having anyway:
// an uncompressed resource is delivered instead of dropped, and a compressed
// one is refused out loud, with a reason an operator can read, instead of
// vanishing between a link that came up and a message that never appeared.
static void onLxmfResourceStarted(const RNS::Resource& resource) {
  // A resource is assembled in RAM before any of it is a message, and the
  // size is the sender's claim. On a node with a few kilobytes of
  // byte-addressable RAM left, believing an arbitrary claim is how a stranger
  // reboots it. The cap is well above any administrative command or written
  // message and far below what would hurt; past it the transfer is cancelled
  // rather than attempted, and said out loud so an operator can see why a
  // large message did not arrive.
  const size_t size = const_cast<RNS::Resource&>(resource).get_data_size();
  if (size > kLxmfResourceMax) {
    log_w("lxmf: refusing a %u byte transfer (this node accepts up to %u); cancelled",
          (unsigned)size, (unsigned)kLxmfResourceMax);
    const_cast<RNS::Resource&>(resource).cancel();
  }
}

static void onLxmfResourceConcluded(const RNS::Resource& resource) {
  if (const_cast<RNS::Resource&>(resource).status() != RNS::Type::Resource::COMPLETE) {
    log_w("lxmf: a transfer over a link did not complete; nothing to read");
    return;
  }
  // No proof to send here. A resource carries its own, exchanged as the
  // transfer concludes, so the sender's client already knows it arrived.
  // A resource carries no per-packet reading of its own, and the last
  // packet's is not the transfer's. Nothing is claimed rather than
  // something wrong being claimed.
  handleLxmfMessage(const_cast<RNS::Resource&>(resource).data(), Rns::ViaResource, {});
}

// A client that has delivered a message over a link then tells the link who
// it was — LXMF calls this the backchannel, and it is how the two ends reply
// to each other over one link instead of each opening its own.
//
// It is also the answer to the other half of why nothing verifies. A message
// can only be checked against the sender's public key, and until now the only
// way to hold one was to have heard that sender announce — which a node that
// has just rebooted has not done for anybody, so every message it took was
// marked unverified however honest it was.
//
// An identify carries the key itself, with a signature over the link id that
// microReticulum checks before this is called (Link.cpp), so it is proof and
// not a claim. And the address it belongs to is not the sender's to choose:
// an lxmf.delivery hash is derived from the key, so remembering the pair
// cannot be used to speak for somebody else's address. Remembering it is
// therefore exactly what hearing the announce would have done, and it makes
// this sender's next message — over this link, another link, or a single
// packet — one this node can actually check.
//
// It does not rescue the message that came before it, and should not: what
// standing a message has is a fact about the moment it arrived.
static void onLxmfIdentified(const RNS::Link& link, const RNS::Identity& identity) {
  (void)link;
  if (!identity) return;
  const Bytes source = RNS::Destination::hash_from_name_and_identity("lxmf.delivery", identity);
  const bool isNew = !RNS::Identity::recall(source) && !provenKeyFor(source.data());
  rememberProvenKey(source, identity.get_public_key());
  if (isNew)
    log_i("lxmf: %s identified itself on its link; this node can now check what it sends",
          source.toHex().c_str());
}

static void onLxmfLink(RNS::Link& link) {
  log_i("lxmf: a client opened a link to the delivery address");
  link.set_packet_callback(onLxmfLinkPacket);
  link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
  link.set_resource_started_callback(onLxmfResourceStarted);
  link.set_resource_concluded_callback(onLxmfResourceConcluded);
  link.set_remote_identified_callback(onLxmfIdentified);
}

// A message sent as one packet straight at the delivery address — which is
// what a client uses for something short when it already has a path — does
// not carry the destination hash. The sender strips it, because the receiver
// is the destination and already knows it; LXMF's own router puts it back
// before parsing, and so must this.
//
// Without that the parse ran sixteen bytes out of step and failed, so the
// message was counted as "not an LXMF message", never proved, and the sender
// was left showing it undelivered. Only this callback needs it: it is
// registered on the destination, and microReticulum reaches it only for
// packets addressed to the destination itself. Link packets arrive whole,
// through onLxmfLinkPacket.
static void onLxmfPacket(const RNS::Bytes& data, const RNS::Packet& packet) {
  RNS::Bytes whole(lxmfDest.hash());
  whole.append(data);
  proveIfTaken(packet, handleLxmfMessage(whole, Rns::ViaPacket, signalOf(packet)));
}

const char* nomadAddress() {
  static char hex[33] = "";
  if (sStarted && nomadDest) strlcpy(hex, nomadDest.hash().toHex().c_str(), sizeof(hex));
  return hex;
}

LxmfState lxmf() {
  LxmfState s{};
  s.received = sLxmfRx;
  s.rejected = sLxmfRejected;
  s.unverified = sLxmfUnverified;
  s.mismatched = sLxmfMismatched;
  s.notStored = Rns::Inbox::dropped();
  if (sStarted && lxmfDest) strlcpy(s.address, lxmfDest.hash().toHex().c_str(), sizeof(s.address));
  return s;
}

// Answering a message, which is the same three steps as reading one, backwards:
// build the payload, sign the bytes LXMF signs, put the envelope on the wire.
// Every part of the layout comes from LxmfFormat.h — the file that learned,
// over three bugs, exactly which bytes those are — so the two directions
// cannot come to different conclusions about it.
// One answer waiting to go out. microReticulum keeps its tables in plain std::
// containers with no lock of its own, and everything else in this firmware
// enters it from the Reticulum task only — so a reply composed on the loop
// task is handed over rather than sent there. The inbox does the same thing in
// the other direction, for the same reason.
// What this node can honestly say about itself right now.
//
// Gathered here, where the hardware is, so Telemetry.h stays a pure question
// about shape. Each reading is offered only where the board can actually take
// it: a node with no cell says nothing about a battery rather than reporting
// zero percent, and a receiver with no fix reports no position rather than the
// Atlantic. What a client cannot see it does not have to disbelieve.
// Any clock reading before this is uptime rather than a date: nothing here
// sets the clock except a GNSS receiver, so a board without one counts from
// the epoch at boot. 2025-01-01, comfortably before any node this firmware
// runs on was built and comfortably after anything a counter reaches.
static const uint64_t kClockIsRealAfter = 1735689600ULL;

static Rns::Telemetry::Snapshot telemetrySnapshot(const Rns::Commands::Signal& signal) {
  Rns::Telemetry::Snapshot s;
  // Only a clock somebody set. Nothing here runs NTP, and a node without a
  // GNSS receiver has never had its clock set at all — time() then counts from
  // the epoch at boot, so two minutes in it says 1970-01-01 00:02. That is not
  // a slightly wrong reading: Sideband keys every stored entry and every point
  // it plots by this timestamp, so it would file the node's whole history in
  // 1970 and place every other reading with it. Anything before this floor is
  // uptime wearing a date.
  const uint64_t now = (uint64_t)time(nullptr);
  s.utc = now > kClockIsRealAfter ? now : 0;

  snprintf(s.information, sizeof(s.information), "RetiMesh Node %s (%s)",
           FW_VERSION, BOARD_NAME);

  const Power::Battery b = Power::battery();
  if (b.present) {
    s.haveBattery = true;
    s.batteryPercent = (float)b.percent;
    s.charging = b.charging;
    // Whether the board can see its charger at all, carried through rather
    // than flattened into a false (Power.h).
    s.chargeKnown = b.chargeKnown;
  }

  // Only with the operator's say-so. A node's position is the operator's to
  // publish, and the setting that governs the public status API governs this
  // for the same reason — it is the same disclosure to a wider audience.
  const Gps::Fix fix = Gps::fix();
  if (fix.valid && settings.radio().gpsSharePosition && s.utc) {
    s.havePosition = true;
    s.latitude = fix.latitude;
    s.longitude = fix.longitude;
    s.altitudeM = fix.altitude;
    s.speedKmh = fix.speedKmh;
    // HDOP is a dilution figure, not a distance. Five metres per unit is the
    // usual rule of thumb for a consumer receiver, and it is offered as the
    // estimate it is rather than left out — a position with no accuracy at all
    // reads as exact.
    s.accuracyM = fix.hdop * 5.0f;
    s.positionAt = s.utc > fix.ageMs / 1000 ? s.utc - fix.ageMs / 1000 : s.utc;
  }

  // What was heard of the message that asked, carried from there rather than
  // read from the radio now. A request that arrived over Wi-Fi or a link has
  // no reading at all, and says so instead of borrowing the last LoRa frame's.
  if (!isnan(signal.rssi) || !isnan(signal.snr)) {
    s.haveSignal = true;
    s.rssi = isnan(signal.rssi) ? 0.0f : signal.rssi;
    s.snr  = isnan(signal.snr) ? 0.0f : signal.snr;
    s.quality = isnan(signal.q) ? 0 : (uint8_t)signal.q;
  }

  s.haveProcessor = true;
  s.cpuHz = (uint64_t)getCpuFrequencyMhz() * 1000000ULL;

  // Byte-addressable internal RAM, which is the figure that decides whether a
  // node survives (Diag.h) — not the total including PSRAM, which would look
  // healthy on a board that is about to run out of the kind it needs.
  const uint32_t heapFree = ESP.getFreeHeap();
  const uint32_t heapAll  = ESP.getHeapSize();
  if (heapAll) {
    s.haveMemory = true;
    s.heapCapacity = heapAll;
    s.heapUsed = heapAll - heapFree;
  }

  // Internal flash, and the card as its own entry where the store lives there.
  // Reporting only the first showed an operator a figure that never moved
  // while the card filled underneath it.
  //
  // Cached, because LittleFS::usedBytes() walks the filesystem to count used
  // blocks and holds its lock while it does. This runs on the task that drains
  // the LoRa receive ring, and a remote peer can ask for telemetry every ten
  // seconds — a traversal on that path is a stranger's way of making the node
  // drop frames. A minute-old figure is a perfectly good answer to "how full
  // is it".
  static uint32_t storageAtMs = 0;
  static uint64_t flashAll = 0, flashUsed = 0;
  if (!storageAtMs || (uint32_t)(millis() - storageAtMs) > 60000) {
    flashAll = LittleFS.totalBytes();
    flashUsed = flashAll ? LittleFS.usedBytes() : 0;
    storageAtMs = millis();
  }
  if (flashAll) {
    s.haveStorage = true;
    s.flashCapacity = flashAll;
    s.flashUsed = flashUsed;
  }
#if HAS_SD
  // The card's own figures are sampled by the card driver already, so asking
  // costs nothing here.
  const SdCard::Info card = sdCard.info();
  if (card.volumeBytes) {
    s.haveCard = true;
    s.cardCapacity = card.volumeBytes;
    s.cardUsed = card.usedBytes;
  }
#endif
  return s;
}

// `telemetry` asks for the node's own readings to be attached. The document
// is not carried here: it is built when the answer goes out, so the queue
// stays small on a board that has little to spare, and — more to the point —
// so the readings are the ones at the moment of sending rather than at the
// moment of asking.
// The signal travels with the answer because it is a fact about the packet
// that asked, not about the node at the moment of replying. Read from the
// radio's latest instead, a telemetry answer reported whatever frame arrived
// in between — so a peer at the edge of coverage was told it had a perfect
// link, because a neighbour two metres away had spoken since. That is the
// same fault the frame carrier fixed on the way in.
struct PendingReply {
  uint8_t  dest[16];
  char     text[256];
  bool     telemetry;
  Rns::Commands::Signal signal;
  // The key the request was verified against, where there was one. Carrying it
  // saves the second Identity::recall — which can reach the SD card, on the
  // task that also drives reticulum.loop() — and settles a question the second
  // lookup could get a different answer to: the reply goes to the identity
  // that was checked, not to whatever that hash resolves to a pass later.
  uint8_t  key[kPublicKeyLen];
  bool     haveKey;
};
static QueueHandle_t sReplyQueue = nullptr;

// The outbound log (RnsTransport.h): written by whoever queues, stamped by
// the RNS task, read by the display — a spinlock because every touch is a
// short copy.
static OutMessage    sOutLog[8];
static uint32_t      sOutCount = 0;
static portMUX_TYPE  sOutMux = portMUX_INITIALIZER_UNLOCKED;

bool queueLxmfReply(const uint8_t destHash[16], const char* text) {
  return queueLxmfTelemetry(destHash, text, false, {});
}

bool queueLxmfTelemetry(const uint8_t destHash[16], const char* text, bool telemetry,
                        const Rns::Commands::Signal& signal, const uint8_t* verifiedKey) {
  if (!sReplyQueue) return false;
  PendingReply r{};
  memcpy(r.dest, destHash, 16);
  strlcpy(r.text, text ? text : "", sizeof(r.text));
  r.telemetry = telemetry;
  r.signal = signal;
  if (verifiedKey) {
    memcpy(r.key, verifiedKey, kPublicKeyLen);
    r.haveKey = true;
  }
  const bool queued = xQueueSend(sReplyQueue, &r, 0) == pdTRUE;
  if (queued && !telemetry) {
    taskENTER_CRITICAL(&sOutMux);
    OutMessage& o = sOutLog[sOutCount % 8];
    memcpy(o.dest, destHash, 16);
    strlcpy(o.text, text ? text : "", sizeof(o.text));
    o.queuedMs = millis() ? millis() : 1;
    o.sentMs = 0;
    o.ok = false;
    sOutCount++;
    taskEXIT_CRITICAL(&sOutMux);
  }
  return queued;
}

size_t lxmfOutbound(OutMessage* out, size_t max) {
  taskENTER_CRITICAL(&sOutMux);
  const uint32_t n = sOutCount < 8 ? sOutCount : 8;
  size_t w = 0;
  for (uint32_t i = 0; i < n && w < max; i++)
    out[w++] = sOutLog[(sOutCount - 1 - i) % 8];   // newest first
  taskEXIT_CRITICAL(&sOutMux);
  return w;
}

static bool sendLxmf(const uint8_t destHash[16], const char* text, bool telemetry,
                     const Rns::Commands::Signal& signal,
                     const uint8_t* verifiedKey = nullptr) {
  if (!sStarted || !lxmfDest) return false;
  RNS::Identity peer(false);
  if (verifiedKey) peer.load_public_key(RNS::Bytes(verifiedKey, kPublicKeyLen));
  else             peer = senderKey(destHash);
  if (!peer) {
    log_w("lxmf: no key for %s; cannot answer", RNS::Bytes(destHash, 16).toHex().c_str());
    return false;
  }

  uint8_t payload[320];
  // The node's own clock, which on a board without a GNSS receiver is time
  // since boot counted from 1970. That is honest — it is what this node knows
  // — and nothing depends on it: the freshness rule applies to messages
  // arriving, where the sender's clock is the one that matters.
  // The node's readings, where the answer is to a request for them. Built now
  // rather than when the request arrived, so what goes out is current.
  uint8_t fields[288];
  size_t flen = 0;
  if (telemetry) {
    flen = Rns::Telemetry::fields(telemetrySnapshot(signal), fields, sizeof(fields));
    if (!flen) log_w("lxmf: the telemetry document did not fit; answering without it");
  }
  const size_t plen = Rns::lxmfPayload((double)time(nullptr), "", text, payload, sizeof(payload),
                                       flen ? fields : nullptr, flen);
  if (!plen) return false;

  // Signed through the same spans the receiving side hashes, so the two cannot
  // disagree about the order — which is the mistake this file has already made
  // once, and the one that makes an honest sender look like a forger. Nothing
  // sent from here is stamped, so what is signed is the whole of what is sent.
  const RNS::Bytes selfHash = lxmfDest.hash();
  const Rns::LxmfMessage outgoing = Rns::lxmfOutgoing(destHash, selfHash.data(), payload, plen);
  RNS::Bytes hashedPart;
  Rns::lxmfSignedSpans(outgoing, [&](const uint8_t* p, size_t n) { hashedPart.append(p, n); });
  RNS::Bytes signedData(hashedPart);
  signedData.append(RNS::Identity::full_hash(hashedPart));
  const RNS::Bytes sig = nodeRnsIdentity.sign(signedData);
  if (sig.size() != 64) return false;

  // Opportunistic, so the destination hash is left off: the destination it
  // arrives at is what says which one it was, and the sixteen bytes are worth
  // more as message.
  uint8_t envelope[16 + 64 + sizeof(payload)];
  const size_t elen = Rns::lxmfEnvelope(destHash, selfHash.data(), sig.data(), payload, plen,
                                        envelope, sizeof(envelope), /*includeDest*/ false);
  if (!elen) return false;

  RNS::Destination out(peer, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                       "lxmf", "delivery");
  RNS::Packet packet(out, RNS::Bytes(envelope, elen));
  packet.send();
  const bool sent = packet.sent();
  if (!sent) log_w("lxmf: could not send the answer to %s",
                   RNS::Bytes(destHash, 16).toHex().c_str());
  return sent;
}

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing) {
  sTxRing = txRing; sRxRing = rxRing; sTcpInRing = tcpInRing;
  // Deep enough that every interface this node can hold could register at
  // once and still leave room for churn. Eight was under half of that, and a
  // full queue lost peers silently.
  sEvents = xQueueCreate(RNS_MAX_INTERFACES + 8, sizeof(Event));
  sReplyQueue = xQueueCreate(2, sizeof(PendingReply));
  sSnapLock = xSemaphoreCreateMutex();
  // Take the room for the snapshots now. These used to be local vectors that
  // grew from empty on every refresh: a doubling cascade of allocate-and-free
  // every five seconds, which is what chopped the heap into pieces too small
  // to hold the next cascade. A Wireless Paper reached 19 KB free with a
  // largest block of 1652 B and threw std::bad_alloc out of the Reticulum loop
  // on every pass from then on, while still serving HTTP and looking healthy.
  // Reserved here, at startup, the allocation happens once and does not have
  // to be found again later.
  //
  // Guarded because this is a few kilobytes wanted before the transport is
  // even up, and on the smallest boards that can fail. Failing here must cost
  // the fragmentation fix, not the boot: reserve() throwing out of begin()
  // would leave setup() with nothing to catch it and abort the node. When it
  // does fail the vectors simply grow on demand, as they always did.
  Diag::guard("reserving the snapshot buffers", [] {
    sPaths.reserve(SNAPSHOT_MAX_PATHS);   sPathsStaging.reserve(SNAPSHOT_MAX_PATHS);
    sIfaces.reserve(RNS_MAX_INTERFACES);  sIfacesStaging.reserve(RNS_MAX_INTERFACES);
  });

  if (!settings.transport().enabled) { log_w("Reticulum transport disabled in settings"); return false; }

  try {
    RNS::loglevel(kLogLevel);            // DEBUG is compiled in; raise kLogLevel when tracing

    // Storage: the SD card or the LittleFS partition shared with the web app.
    // StoreHome owns that rule, the card's ownership marker and the filesystem
    // the store is pointed at, and it has already applied all three — in
    // setup(), before this or anything else could open a file. The choice used
    // to be made here, which put it behind the enabled check a few lines up: a
    // node with the transport switched off never chose, so every answer about
    // the store's home was the default one. It said flash while the store sat
    // on the card, the settings page offered to adopt a card that was already
    // the home, and the boot after that copied the flash tree over the real
    // one. Nothing to re-derive here; rnsFs already points at the home.
    rnsFs.init(false);
    log_i("Reticulum storage: %s%s (%u KB free)", RnsFileSystem::prefix(), RNS_FS_ROOT,
          (unsigned)(rnsFs.storageAvailable() / 1024));
    logStoreContents();
    RNS::Utilities::OS::register_filesystem(rnsFs);
    reticulum = RNS::Reticulum();          // ctor resets the storage path, so set it after
    RNS::Reticulum::storagepath(RNS_FS_ROOT);
    // After the filesystem is registered and the store path is settled, so
    // the inbox lands beside the transport's own files wherever those went.
    Rns::Inbox::begin();
    // After the inbox, which it reads to find where each administrator got to
    // before the restart (RnsAdmin.h).
    Rns::Admin::begin();
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
    lxmfDest = RNS::Destination(nodeRnsIdentity, RNS::Type::Destination::IN,
                                RNS::Type::Destination::SINGLE, "lxmf", "delivery");
    lxmfDest.set_packet_callback(onLxmfPacket);
    lxmfDest.set_link_established_callback(onLxmfLink);
    log_i("lxmf: this node can be messaged at %s", lxmfDest.hash().toHex().c_str());

    // The page. NomadNet asks for /page/index.mu on a bare node address, so
    // that is the one path a browser needs to find anything here.
    nomadDest = RNS::Destination(nodeRnsIdentity, RNS::Type::Destination::IN,
                                 RNS::Type::Destination::SINGLE, "nomadnetwork", "node");
    nomadDest.register_request_handler("/page/index.mu", serveIndex,
                                       RNS::Type::Destination::ALLOW_ALL);
    log_i("nomadnet: this node can be browsed at %s", nomadDest.hash().toHex().c_str());
    sStarted = true;
    sAnnounceFloorMs = millis() + ANNOUNCE_BOOT_DELAY_MS;
    sNextAnnounceMs  = sAnnounceFloorMs;
    const TransportSettings& t = settings.transport();
    log_i("Reticulum transport up: identity %s, modes LoRa %s, clients %s, auto peers %s",
          RNS::Transport::identity().hash().toHex().c_str(),
          modeName(t.loraMode), modeName(t.wifiMode), modeName(t.autoMode));
    // RNS refuses to broadcast announces onto an access_point interface, and
    // says so only at a TRACE level this build does not compile in. Left
    // unsaid, the result is a node that looks alive and whose neighbours never
    // hear of it, so say it once at boot, where it will be read.
    const struct { const char* who; uint8_t mode; } kinds[] = {
      { "the radio",            t.loraMode },
      { "clients on the port",  t.wifiMode },
      { "AutoInterface peers",  t.autoMode },
    };
    for (const auto& k : kinds)
      if (blocksAnnounces(k.mode))
        log_w("no announces are sent to %s: mode is access_point, so they must discover by path request", k.who);
  } catch (const std::exception& e) {
    log_e("Reticulum transport start failed: %s", e.what());
  }
  return sStarted;
}

// Both of these are called from the AsyncTCP and AutoInterface tasks, and both
// used to post into the queue without looking at the answer. A dropped connect
// is a peer whose packets drainTcp() then discards for as long as it stays
// connected; a dropped disconnect is an interface registered for the rest of
// the uptime, pointing at a socket that has gone. Neither said anything.
static void postEvent(const Event& e) {
  if (!sEvents) {
    // The listener can be accepting before begin() has built the queue.
    log_w("transport event for #%lu dropped: the RNS task is not up yet", (unsigned long)e.id);
    return;
  }
  if (xQueueSend(sEvents, &e, pdMS_TO_TICKS(20)) != pdTRUE)
    log_e("transport event queue full, dropped %s for #%lu",
          e.connect ? "connect" : "disconnect", (unsigned long)e.id);
}

void clientConnected(uint32_t id, const char* remote) {
  Event e{ true, id, {0} };
  strlcpy(e.remote, remote, sizeof(e.remote));       // "ip:port", or an Auto peer's link-local
  postEvent(e);
}

void clientDisconnected(uint32_t id) {
  Event e{ false, id, {0} };
  postEvent(e);
}

// Bring the next announce forward, never push it back, and never move it
// before the boot delay — a peer that turns up while the radio is still
// starting has to wait for the radio like everyone else. Announces switched
// off in settings stay off: loop() checks the interval, not this.
static void announceSoon() {
  if (!sStarted) return;
  uint32_t at = millis() + ANNOUNCE_ON_PEER_DELAY_MS;
  if ((int32_t)(at - sAnnounceFloorMs) < 0) at = sAnnounceFloorMs;
  if ((int32_t)(at - sNextAnnounceMs) < 0) sNextAnnounceMs = at;
}

void announceNow() { announceSoon(); }

// Drop whatever is registered under this interface's hash. Names are unique
// per registration now, so this only fires if one ever repeats — but a
// collision costs the new peer every packet it should have received, silently
// and for good, which is too expensive to leave to the naming scheme alone.
static void evictCollision(const RNS::Interface& iface) {
  if (!RNS::Transport::find_interface_from_hash(iface.get_hash())) return;
  log_w("interface name %s is already registered; dropping the older one",
        iface.name().c_str());
  for (auto it = tcpIfaces.begin(); it != tcpIfaces.end(); ++it) {
    if (it->second.handle.get_hash() != iface.get_hash()) continue;
    RNS::Transport::deregister_interface(it->second.handle);
    tcpIfaces.erase(it);
    return;
  }
  RNS::Transport::deregister_interface(iface);      // not ours: the LoRa interface
}

static void processEvents() {
  Event e;
  while (xQueueReceive(sEvents, &e, 0) == pdTRUE) {
    if (e.connect) {
      char name[INTERFACE_NAME_MAX];
      interfaceName(e.id, e.remote, name, sizeof(name));
      auto* impl = new TcpClientRnsInterface(e.id, name);
      RNS::Interface iface(impl);
      const RNS::Type::Interface::modes mode = modeFor(e.id);
      iface.mode(mode);
      evictCollision(iface);
      // All three steps or none. start() opens sockets and emplace() allocates
      // a map node, so either can throw — and the pass that calls this is
      // guarded, so a throw between them did not restart the node: it left an
      // interface registered with Transport and absent from tcpIfaces, which
      // drainTcp cannot route to and clientDisconnected cannot ever remove.
      // The snapshot then listed it as live, with counters, for the rest of
      // the node's uptime.
      RNS::Transport::register_interface(iface);
      try {
        iface.start();
        tcpIfaces.emplace(e.id, TcpIface{ iface, impl });
      } catch (...) {
        RNS::Transport::deregister_interface(iface);
        log_w("could not bring up %s; left nothing half-registered", name);
        throw;                        // counted and contained by the guard above
      }
      // A neighbour that has just appeared has heard nothing from this node,
      // and the periodic announce can be ten minutes away. Announce shortly,
      // once, however many peers arrive together — which is what makes a
      // phone that has only just connected see the node at all.
      announceSoon();
      log_i("registered %s (%s)", name, modeNameOf(mode));
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
    // As in the radio drain above: incoming() runs a whole packet through the
    // library and can throw, and a hand-written return is skipped when it
    // does — bleeding this ring until no client's traffic gets through.
    Sys::RingItem held(sTcpInRing, item);
    if (sz > sizeof(TcpItemHeader)) {
      TcpItemHeader h; memcpy(&h, item, sizeof(h));
      auto it = tcpIfaces.find(h.clientId);
      if (it != tcpIfaces.end()) it->second.impl->incoming(item + sizeof(h), sz - sizeof(h));
    }
  }
}

// Snapshots for the web layer
static uint32_t sSnapAtMs = 0;
static uint32_t sSnapOkMs = 0;          // last pass that actually published; the age readers see
// Dead paths are cleaned up on a slower clock than the reading is refreshed:
// the reading is capped and cheap, the sweep walks the whole table.
static const uint32_t kStaleSweepMs = 60000;
static size_t   sPathCount = 0;          // full table size; sPaths is capped
static size_t   sIfaceCount = 0;         // likewise, so a capped list still counts true
static Tables   sTables = {};            // table sizes, for soak monitoring

static void refreshSnapshots(bool allowSweep) {
  // Walking the path table reads every record back through microStore
  // (LittleFS or the SD card), so refresh it slowly and keep the list short —
  // the count is free, the rows are not.
  if (millis() - sSnapAtMs < SNAPSHOT_INTERVAL_MS) return;
  // Stamped on the way in, so a pass that throws does not come straight back
  // on the next 10 ms tick and spend the node's remaining memory logging about
  // it. What it must not do is imply the reading is current: sSnapOkMs, set
  // only after the publish below, is what /api/status reports the age from, so
  // a snapshot that stopped updating says so instead of looking fresh.
  sSnapAtMs = millis();
  // clear() keeps the capacity reserved at begin(), so the push_back()s below
  // write into memory this node already owns.
  std::vector<PathInfo>&  p = sPathsStaging;
  std::vector<IfaceInfo>& i = sIfacesStaging;
  p.clear();
  i.clear();
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
  // The last list here that still grows from empty. It is normally empty and
  // costs nothing, but a Wi-Fi client that disconnects leaves one dead path
  // behind per destination it carried, so on a busy node this is the same
  // doubling cascade the other two just stopped doing. Bounded to one pass's
  // worth: what is not dropped this time is dropped in five seconds.
  //
  // On its own, slower clock. Collecting rows stops at SNAPSHOT_MAX_PATHS, and
  // the scan used to stop with it — so on a node with more live paths than
  // that, every entry past the sixty-fourth was never examined and a dead one
  // sitting there was dropped never rather than in five seconds. Iteration
  // order is stable, so it stopped in the same place every pass, for ever.
  //
  // The whole table is walked when a sweep is due and the rows are capped as
  // before when it is not, which keeps the reading cheap at five seconds and
  // the cleanup complete at a minute.
  static std::vector<RNS::Bytes> stale;
  static uint32_t sSweptMs = 0;
  const bool sweeping = allowSweep && (uint32_t)(millis() - sSweptMs) >= kStaleSweepMs;
  stale.clear();
  for (auto it = pathTable.begin(); it != pathTable.end(); ++it) {
    RNS::Persistence::NewPathTable::Entry& e = *it;
    if (!e.value.receiving_interface()) {
      if (sweeping && stale.size() < SNAPSHOT_MAX_PATHS) stale.push_back(e.key);
      continue;
    }
    if (p.size() >= SNAPSHOT_MAX_PATHS) continue;    // counted by the table, swept above, not rendered
    PathInfo pi = {};
    strlcpy(pi.hash, e.key.toHex().c_str(), sizeof(pi.hash));
    pi.hops = e.value._hops;
    strlcpy(pi.via, e.value.receiving_interface() ? e.value.receiving_interface().name().c_str() : "?", sizeof(pi.via));
    pi.ageS = (uint32_t)max(0.0, now - e.value._timestamp);
    if (p.size() < SNAPSHOT_MAX_PATHS) p.push_back(pi);
    else if (!sweeping) break;              // rows are full and nothing else needs the rest
  }
  if (sweeping) sSweptMs = millis();
  if (!stale.empty()) {
    uint16_t dropped = RNS::Transport::remove_paths(stale);
    log_i("dropped %u stored path(s) whose interface is gone", (unsigned)dropped);
  }
  // Counted into locals and published below with the lists they describe. As
  // members they were written here, outside the lock every reader takes, and
  // sIfaceCount was zeroed and counted back up mid-scan — so a panel or a
  // status request landing in that window read "0 interfaces" while sIfaces
  // still held the full previous list.
  const size_t pathTotal = pathTable.size();
  size_t ifaceCount = 0;
  for (const auto& iface : RNS::Transport::get_interfaces()) {
    IfaceInfo ii = {};
    strlcpy(ii.name, iface.name().c_str(), sizeof(ii.name));
    strlcpy(ii.mode, modeNameOf(iface.mode()), sizeof(ii.mode));
    ii.rxb = iface.rxbytes(); ii.txb = iface.txbytes();
    ifaceCount++;
    if (i.size() < RNS_MAX_INTERFACES) i.push_back(ii);   // never grow past what was reserved
  }
  // Sizes only — every one of these is a container the RNS task owns, so they
  // are read here and published under the same lock as the rest, never touched
  // from the web task.
  Tables t = {};
  t.paths         = (uint32_t)pathTotal;
  t.links         = (uint32_t)RNS::Transport::link_table().size();
  t.activeLinks   = (uint32_t)RNS::Transport::active_links().size();
  t.pendingLinks  = (uint32_t)RNS::Transport::pending_links().size();
  t.destinations  = (uint32_t)RNS::Transport::destinations().size();
  t.announces     = (uint32_t)RNS::Transport::announce_table().size();
  t.heldAnnounces = (uint32_t)RNS::Transport::held_announces().size();
  t.rates         = (uint32_t)RNS::Transport::announce_rate_table().size();

  // Sys::Lock, not a hand-written pair. Lock.h states the rule: this whole
  // function now runs under Diag::guard, so there is a way out of the scope
  // that a give at the bottom does not cover, and a mutex leaked that way
  // deadlocks the web task, the display task and this one on the next pass.
  // Nothing in here throws today; the point is that it no longer has to be
  // checked before anything is added.
  Sys::Lock held(sSnapLock);
  sPaths.swap(p); sIfaces.swap(i);
  sPathCount = pathTotal; sIfaceCount = ifaceCount;
  sTables = t;
  sSnapOkMs = millis();                 // only here: a pass that threw never reaches this
}

size_t paths(PathInfo* out, size_t max) {
  Sys::Lock held(sSnapLock);
  size_t n = min(max, sPaths.size());
  for (size_t k = 0; k < n; k++) out[k] = sPaths[k];
  return n;
}

size_t interfaces(IfaceInfo* out, size_t max) {
  Sys::Lock held(sSnapLock);
  size_t n = min(max, sIfaces.size());
  for (size_t k = 0; k < n; k++) out[k] = sIfaces[k];
  return n;
}

size_t pathCount() {
  Sys::Lock held(sSnapLock);
  return sPathCount;                     // whole table, even when the list is capped
}

size_t interfaceCount() {
  Sys::Lock held(sSnapLock);
  return sIfaceCount;                    // the whole list, even when sIfaces is capped
}

Tables tables() {
  Sys::Lock held(sSnapLock);
  return sTables;
}

// Everything a caller needs to describe the node's tables, out of one pass.
//
// Taken separately, a count and the list it counts came from different
// refreshes: a panel read interfaceCount() as five, a refresh landed, and
// interfaces() then returned three rows — so it printed "+2 more" for
// interfaces that no longer existed. Moving the counts inside the lock fixed
// who could see a half-written pass, not who could see two.
Snapshot snapshot(PathInfo* pathOut, size_t maxPaths,
                  IfaceInfo* ifaceOut, size_t maxIfaces) {
  Sys::Lock held(sSnapLock);
  Snapshot s = {};
  s.tables      = sTables;
  s.pathTotal   = sPathCount;
  s.ifaceTotal  = sIfaceCount;
  s.pathRows    = pathOut  ? min(maxPaths,  sPaths.size())  : 0;
  s.ifaceRows   = ifaceOut ? min(maxIfaces, sIfaces.size()) : 0;
  for (size_t k = 0; k < s.pathRows;  k++) pathOut[k]  = sPaths[k];
  for (size_t k = 0; k < s.ifaceRows; k++) ifaceOut[k] = sIfaces[k];
  s.ageMs = (uint32_t)(millis() - sSnapOkMs);
  return s;
}

void loop() {
  if (!sStarted) { vTaskDelay(pdMS_TO_TICKS(500)); return; }
  // Diag::guard rather than a try/catch written out here. Containment is one
  // rule and it lives in one place (Diag.h): what it caught is counted into
  // faults.contained — which is how an operator learns this task is failing,
  // a log line on a node nobody is attached to not being an answer — and,
  // unlike the bare `catch (const std::exception&)` this replaces, it also
  // catches what does not derive from std::exception. Spelled out here, such
  // a throw escaped to the task loop in main.cpp, which abandoned the rest of
  // the pass — refreshSnapshots() included, so the stale snapshot below was
  // served as current after all.
  const bool passOk = Diag::guard("rns loop", [] {
    applyLogMute();
    // Answers other tasks composed, sent from here because this is the task
    // that owns the library (queueLxmfReply above).
    if (sReplyQueue) {
      PendingReply r;
      if (xQueueReceive(sReplyQueue, &r, 0) == pdTRUE) {
        const bool ok = sendLxmf(r.dest, r.text, r.telemetry, r.signal,
                                 r.haveKey ? r.key : nullptr);
        if (!ok) log_w("lxmf: could not send an answer to %s",
                       RNS::Bytes(r.dest, 16).toHex().c_str());
        if (!r.telemetry) {
          // The queue is FIFO, so this send belongs to the oldest unsent
          // log entry for the same destination.
          taskENTER_CRITICAL(&sOutMux);
          const uint32_t from = sOutCount > 8 ? sOutCount - 8 : 0;
          for (uint32_t i = from; i < sOutCount; i++) {
            OutMessage& o = sOutLog[i % 8];
            if (!o.sentMs && memcmp(o.dest, r.dest, 16) == 0) {
              o.sentMs = millis() ? millis() : 1;
              o.ok = ok;
              break;
            }
          }
          taskEXIT_CRITICAL(&sOutMux);
        }
      }
    }
    processEvents();
    drainTcp();
    reticulum.loop();                      // interface loops + housekeeping (jobs_interval = 1 s)

    // sStarted is what says the schedule is valid; testing sNextAnnounceMs for
    // truth as well used to mean that the one millis() in every 49 days that
    // lands the next announce on zero switched announcing off for good.
    uint16_t interval = settings.radio().announceInterval;
    // A booked announce does not survive the interval being lowered. Nothing
    // on the settings path touches the schedule, so a node at the 12-hour
    // maximum dropped to a minute — which is what an operator does when
    // neighbours cannot see it — went on saying nothing for up to 13 hours
    // while /api/status reported the new value.
    static uint16_t sBookedFor = 0;
    if (interval && interval != sBookedFor) {
      sNextAnnounceMs = Rns::clampAnnounceTo(sNextAnnounceMs, interval, millis());
      sBookedFor = interval;
    }
    if (interval && (int32_t)(millis() - sNextAnnounceMs) >= 0) {
      // Scheduled before it is attempted, not after. Advancing only on the way
      // out meant that an announce which threw left the schedule untouched, so
      // the next pass ten milliseconds later tried the same thing again, threw
      // again, and logged again — for as long as whatever made it throw
      // lasted. A failed announce now waits for the next interval like any
      // other, which is also the only way the node stays quiet enough to
      // recover.
      //
      // Scattered, not on the dot. Two nodes flashed together boot together
      // and would then announce together for as long as they both run: the
      // interval is fixed and nothing else moves the phase, so their packets
      // collide on air every time and one of them is never heard. A tenth of
      // the interval is enough to break the lockstep, and small enough that
      // an operator watching for an announce still sees it about when they
      // expect. Reticulum's own implementation jitters for the same reason.
      sNextAnnounceMs = Rns::nextAnnounceAt(interval, millis(), esp_random());

      char app[64];
      // snprintf returns the length it *would* have written. A 32-character
      // callsign and a git-describe version exceed this buffer, and the
      // untruncated length would have put a stray byte from the stack into a
      // signed announce.
      const int n = snprintf(app, sizeof(app), "%s %s", loraRadio.callsign(), FW_VERSION);
      const size_t appLen = n < 0 ? 0 : ((size_t)n < sizeof(app) - 1 ? (size_t)n : sizeof(app) - 1);
      // Every announce this node makes, including the page's. The counter is
      // how an operator tells a node whose mesh side has quietly stopped from
      // one that is merely quiet, so it must not under-report: an announce
      // that is sent and not counted here reads, from the other end of a
      // status page, exactly like a node that has stopped talking. Which is
      // why each one is counted as it goes out rather than all three summed
      // at the end: summed, a throw from the second announce took the count
      // of the first with it, and the node that had just talked reported that
      // it had not.
      nodeDest.announce(Bytes((const uint8_t*)app, appLen));
      g_stats.announcesTx++;
      // Logged here, with the first announce that actually went out, for the
      // same reason the counter is incremented here. At the end of the block a
      // throw from the second or third announce took the line with it, so a
      // node that had just announced twice left no record of having announced
      // at all — and an operator reading the log concluded it had gone quiet.
      log_i("announced retimesh.node <%s> on all interfaces", nodeIdentity.destHex());
      // And the same node under its LXMF address, so it appears in the
      // clients people actually use. The app_data is the shape LXMF expects
      // rather than the free text above — the two announces describe one node
      // to two audiences (RnsAnnounce.h).
      uint8_t lx[64];
      const size_t lxLen = Rns::lxmfAppData(loraRadio.callsign(), 0, lx, sizeof(lx));
      if (lxLen) { lxmfDest.announce(Bytes(lx, lxLen)); g_stats.announcesTx++; }
      // And as something to browse. NomadNet announces its node name as plain
      // UTF-8 rather than the msgpack array LXMF uses — a third audience, and
      // the third shape, from one node.
      const char* nn = loraRadio.callsign();
      nomadDest.announce(Bytes((const uint8_t*)nn, strlen(nn)));
      g_stats.announcesTx++;
    }
  });

  // Guarded separately from the pass above, and deliberately: the snapshots
  // are what /api/status and the panel read, and while this lived at the end
  // of the block above, any throw earlier in the pass skipped it and left the
  // last complete pass being served as though it were current. A node in that
  // state does not look broken — it looks quiet, with a plausible interface
  // list and counters that have stopped. That is worse than an error, because
  // it is believed: it was read as "no TCP client is attached" while a client
  // was in fact attached and working.
  //
  // The reading is still taken when the pass above failed — that is the whole
  // point of the separate guard — but the dead-path sweep is not. Sweeping
  // deletes from the path table, and doing that straight after an allocation
  // failure means mutating a store whose last operation was abandoned
  // part-way, on a node that has just run out of memory. The reading is what
  // an operator needs; the cleanup can wait a minute.
  Diag::guard("rns snapshots", [passOk] { refreshSnapshots(passOk); });

  // Backing off after a caught failure. This task runs every 10 ms, and
  // Diag::noteCaught walks the heap free list twice to report the figures
  // behind a failure — so a condition that throws every pass produced a
  // hundred catches and two hundred free-list traversals a second, under the
  // allocator's lock, on a node whose problem is almost certainly memory. The
  // log that would tell an operator what happened is also the thing burying
  // it. A quarter second between attempts keeps the report and drops the
  // flood.
  if (!passOk) vTaskDelay(pdMS_TO_TICKS(250));
}

} // namespace RnsTransport
