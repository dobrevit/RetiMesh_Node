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
      // Signal stats are private to InterfaceImpl; the wrapper exposes setters.
      std::shared_ptr<RNS::InterfaceImpl> sp = shared_from_this();
      RNS::Interface self(sp);
      self.r_stat_rssi(g_stats.lastRssi);
      self.r_stat_snr(g_stats.lastSnr);
      handle_incoming(Bytes(item, sz));
      vRingbufferReturnItem(sRxRing, item);
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
struct ProvenKey { uint8_t hash[16]; uint8_t key[32]; uint32_t atMs; bool used; };
static ProvenKey sProven[8];

static const ProvenKey* provenKeyFor(const uint8_t* sourceHash) {
  for (const auto& e : sProven)
    if (e.used && memcmp(e.hash, sourceHash, 16) == 0) return &e;
  return nullptr;
}

static void rememberProvenKey(const Bytes& source, const Bytes& key) {
  if (source.size() != 16 || key.size() != 32) return;
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
  memcpy(slot->key, key.data(), 32);
  slot->atMs = millis();
  slot->used = true;
}

static bool handleLxmfMessage(const RNS::Bytes& data, uint8_t via) {
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
  RNS::Identity sender = RNS::Identity::recall(source);
  if (!sender) {
    // Nothing announced — but the sender may have proved who it is on a link,
    // which is the same proof an announce carries and arrives sooner.
    if (const ProvenKey* proven = provenKeyFor(m.sourceHash)) {
      RNS::Identity fromLink(false);
      fromLink.load_public_key(RNS::Bytes(proven->key, 32));
      sender = fromLink;
    }
  }
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
      // Not refused, and this is a judgement rather than an oversight. A
      // mismatch here has two explanations — someone forged a message, or
      // this node checks it wrongly — and only one of them has been ruled
      // out. Verification is proven both ways against the real LXMF library
      // (a sender it knows verifies; one it does not is taken unverified),
      // and Sideband's messages still fail, which is evidence pointing at
      // this side rather than at Sideband's user.
      //
      // Refusing on that evidence drops a real person's message to make a
      // point about a signature we may be checking wrong. Nothing acts on
      // these yet, so the cost of taking them is that a message is shown
      // whose sender is unproven, which is what "unverified" says. When
      // administration over LXMF lands it will require verified, and this
      // becomes a refusal again — by then the mismatch has to be understood.
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
  if (!Rns::Inbox::note(m.sourceHash,
                        verified ? Rns::StandingVerified
                        : !sender ? Rns::StandingNoKey
                                  : Rns::StandingMismatch,
                        via, m.sentAt, (const char*)text,
                        Rns::utf8TrimLen(text, textLen, Rns::kInboxTextMax), sig))
    log_d("lxmf: not stored (a repeat, or arriving faster than the store is written)");
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
  return true;
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
  proveIfTaken(packet, handleLxmfMessage(data, Rns::ViaLink));
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
  handleLxmfMessage(const_cast<RNS::Resource&>(resource).data(), Rns::ViaResource);
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
  proveIfTaken(packet, handleLxmfMessage(whole, Rns::ViaPacket));
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

bool begin(RingbufHandle_t txRing, RingbufHandle_t rxRing, RingbufHandle_t tcpInRing) {
  sTxRing = txRing; sRxRing = rxRing; sTcpInRing = tcpInRing;
  // Deep enough that every interface this node can hold could register at
  // once and still leave room for churn. Eight was under half of that, and a
  // full queue lost peers silently.
  sEvents = xQueueCreate(RNS_MAX_INTERFACES + 8, sizeof(Event));
  sSnapLock = xSemaphoreCreateMutex();

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
      RNS::Transport::register_interface(iface);
      iface.start();
      tcpIfaces.emplace(e.id, TcpIface{ iface, impl });
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

size_t interfaceCount() {
  xSemaphoreTake(sSnapLock, portMAX_DELAY);
  size_t n = sIfaces.size();             // likewise: a caller may read only a few rows
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
    applyLogMute();
    processEvents();
    drainTcp();
    reticulum.loop();                      // interface loops + housekeeping (jobs_interval = 1 s)

    // sStarted is what says the schedule is valid; testing sNextAnnounceMs for
    // truth as well used to mean that the one millis() in every 49 days that
    // lands the next announce on zero switched announcing off for good.
    uint16_t interval = settings.radio().announceInterval;
    if (interval && (int32_t)(millis() - sNextAnnounceMs) >= 0) {
      char app[64];
      int n = snprintf(app, sizeof(app), "%s %s", loraRadio.callsign(), FW_VERSION);
      nodeDest.announce(Bytes((const uint8_t*)app, (size_t)max(n, 0)));
      // And the same node under its LXMF address, so it appears in the
      // clients people actually use. The app_data is the shape LXMF expects
      // rather than the free text above — the two announces describe one node
      // to two audiences (RnsAnnounce.h).
      uint8_t lx[64];
      const size_t lxLen = Rns::lxmfAppData(loraRadio.callsign(), 0, lx, sizeof(lx));
      if (lxLen) lxmfDest.announce(Bytes(lx, lxLen));
      g_stats.announcesTx += lxLen ? 2 : 1;
      // Scattered, not on the dot. Two nodes flashed together boot together
      // and would then announce together for as long as they both run: the
      // interval is fixed and nothing else moves the phase, so their packets
      // collide on air every time and one of them is never heard. A tenth of
      // the interval is enough to break the lockstep, and small enough that
      // an operator watching for an announce still sees it about when they
      // expect. Reticulum's own implementation jitters for the same reason.
      const uint32_t base = (uint32_t)interval * 1000UL;
      sNextAnnounceMs = millis() + base + (esp_random() % (base / 10 + 1));
      log_i("announced retimesh.node <%s> on all interfaces", nodeIdentity.destHex());
    }
    refreshSnapshots();
  } catch (const std::exception& e) {
    log_e("Reticulum loop: %s", e.what());
  }
}

} // namespace RnsTransport
