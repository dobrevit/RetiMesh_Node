// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See StoreHome.h.

#include "StoreHome.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "RnsFileSystem.h"
#include "Settings.h"
#if HAS_SD
  #include <SD.h>
  #include "SdCard.h"
#endif
#include "RnsAnnounce.h"
#include "WifiManager.h"
#include "Display.h"

namespace StoreHome {

// The identity that goes on the card is the node's Reticulum hash in hex. The
// header cannot see that length without dragging Arduino into the host tests,
// so the two are tied together here, where both are in scope: an identity that
// outgrew the marker field would be truncated on the way to the card and every
// node's own store would read back as somebody else's.
static_assert(kIdentityHexLen == 2 * Rns::HASH_LEN,
              "the marker's identity length is out of step with Rns::HASH_LEN");
static_assert(kIdentityHexLen < sizeof(Marker::node),
              "the marker's owner field cannot hold an identity hex string");

static char sResult[128] = "";

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------
const char* whereName(Where w) { return w == Where::Sd ? "sd" : "littlefs"; }

const char* cardName(Card c) {
  switch (c) {
    case Card::Blank:   return "blank";
    case Card::Ours:    return "ours";
    case Card::Foreign: return "foreign";
    case Card::Legacy:  return "legacy";
    default:            return "none";
  }
}

// Not a variable of its own. The store's filesystem already knows which of the
// two it is pointing at, and a second copy of that fact in this file is a
// second answer that can disagree with the first — which is exactly what a
// page reporting "the store is on the card" while the store was being written
// to flash turned out to be.
Where where() { return RnsFileSystem::onSd() ? Where::Sd : Where::LittleFs; }

const char* lastResult() { return sResult; }

// ---------------------------------------------------------------------------
#if HAS_SD
// ---------------------------------------------------------------------------

// The marker sits beside the event log rather than inside the store, so that
// wiping the store does not silently un-own the card, and so a person putting
// the card in a laptop finds both files in one obvious place.
static const char* kMarkerPath = "/retimesh/store.json";
static const char* kMarkerDir  = "/retimesh";
static const uint8_t kSchema   = 1;

// Where a copy is assembled before it becomes the store. Nothing is ever
// deleted to make room for it.
static const char* kStagingRoot = RNS_FS_ROOT ".new";

static bool sRunning = false;       // a move is being carried out right now

static SemaphoreHandle_t sCacheLock = nullptr;
static Ownership         sCache;

void begin() { if (!sCacheLock) sCacheLock = xSemaphoreCreateMutex(); }

Ownership ownership() {
  if (!sCacheLock) return Ownership{};
  xSemaphoreTake(sCacheLock, portMAX_DELAY);
  Ownership o = sCache;
  xSemaphoreGive(sCacheLock);
  return o;
}

Card card() { return ownership().card; }

bool busy() { return sRunning || settings.transport().pendingMove != Move::None; }

// ---------------------------------------------------------------------------
// The marker
// ---------------------------------------------------------------------------
static bool readMarker(Marker& out) {
  out = Marker{};
  if (!sdCard.mounted()) return false;
  File f = SD.open(kMarkerPath, FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  strlcpy(out.node, doc["node"] | "", sizeof(out.node));
  strlcpy(out.name, doc["name"] | "", sizeof(out.name));
  out.generation = doc["generation"] | 0;
  out.released   = doc["released"] | false;
  out.valid      = out.node[0] != '\0';
  return out.valid;
}

static bool writeMarker(uint32_t generation, bool released) {
  if (!sdCard.mounted()) return false;
  SD.mkdir(kMarkerDir);
  File f = SD.open(kMarkerPath, FILE_WRITE);
  if (!f) return false;
  JsonDocument doc;
  doc["schema"]     = kSchema;
  doc["node"]       = nodeIdentity.identityHex();
  doc["name"]       = wifiManager.hostname();
  doc["generation"] = generation;
  doc["released"]   = released;
  const bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

// Read on the card task, handed out from memory everywhere else. Unconditional
// rather than invalidated when something is known to have changed it: a cache
// with a list of things that must remember to dirty it grows a case that
// forgot, and a status page confidently naming the wrong owner is worse than
// one small read every poll.
void refreshCache() {
  if (!sCacheLock) return;
  Ownership o;
  if (sdCard.mounted()) {
    Marker m;
    const bool valid = readMarker(m);
    o.card = classify(true, valid,
                      valid && strcmp(m.node, nodeIdentity.identityHex()) == 0,
                      m.released, SD.exists(RNS_FS_ROOT));
    strlcpy(o.owner, m.name, sizeof(o.owner));
    o.generation = m.generation;
  }
  xSemaphoreTake(sCacheLock, portMAX_DELAY);
  sCache = o;
  xSemaphoreGive(sCacheLock);
}

// ---------------------------------------------------------------------------
// Moving the files
//
// Both ends are fs::FS, so one routine serves either direction. The store is
// small — index and a few segments per table — so this is plain reads and
// writes with no attempt at being clever about it.
// ---------------------------------------------------------------------------
static uint8_t sBuf[512];

// One path, either end: whether it is there, whether it is a directory, how
// big it is, and what is in it. The names are read out and the handle is
// closed before anything is done with them, which is what removeTree needs:
// deleting through an open directory moves the cursor under the reader, so
// entries get stepped over and a tree that was "removed" keeps some of its
// files — leaving behind a store that is neither the old one nor an empty one.
struct Entry {
  bool                     found = false;
  bool                     isDir = false;
  size_t                   size  = 0;
  std::vector<std::string> names;
};

static Entry entryAt(fs::FS& fs, const char* path) {
  Entry e;
  File f = fs.open(path);
  if (!f) return e;
  e.found = true;
  e.isDir = f.isDirectory();
  if (e.isDir)
    for (File c = f.openNextFile(); c; c = f.openNextFile()) { e.names.push_back(c.name()); c.close(); }
  else
    e.size = f.size();
  f.close();
  return e;
}

// Long, synchronous, and running inside setup() on the main task, where
// nothing else will hand the scheduler a turn. A store of any size takes
// several seconds of solid filesystem work, and with no host reading the
// console there is not even a log write to yield on — so the idle task starves
// and the watchdog panics mid-migration. It only ever survived while somebody
// was watching it on a serial monitor, which is the least helpful kind of bug
// there is. Every loop that walks or copies gives the scheduler a turn.
static inline void breathe() { vTaskDelay(1); }

// Files copied so far, shown on the panel. A count that climbs is the
// difference between a node that is working and a node that has hung, and the
// operator holding it has no other way to tell the two apart.
static uint16_t sCopied = 0;
static void progress(const char* what) {
  char line[24];
  snprintf(line, sizeof(line), "%s %u", what, (unsigned)sCopied);
  display.notice("Moving store", line);
}

static bool copyFile(fs::FS& from, const char* src, fs::FS& to, const char* dst) {
  File in = from.open(src, FILE_READ);
  if (!in) return false;
  const size_t expect = in.size();
  File out = to.open(dst, FILE_WRITE);
  if (!out) { in.close(); return false; }
  size_t done = 0;
  bool ok = true;
  while (ok && done < expect) {
    const size_t want = (expect - done) < sizeof(sBuf) ? (expect - done) : sizeof(sBuf);
    breathe();
    const size_t n = in.read(sBuf, want);
    // A read that returns nothing is not the end of the file — the file's own
    // size says where that is. Stopping at the first short read treated a card
    // that had gone quiet mid-copy as a file that had simply ended, and the
    // truncated result was indistinguishable from a complete one.
    if (n == 0) { ok = false; break; }
    ok = out.write(sBuf, n) == n;
    done += n;
  }
  out.flush();
  out.close();
  in.close();
  if (!ok || done != expect) return false;
  // The length is checked after the handle is closed, not before: a filesystem
  // is entitled to keep it in RAM until then, so asking an open file how big it
  // is answers from wherever it happens to have the number rather than from
  // what reached the medium. This is the only way a flush that failed on the
  // way out gets noticed at all — close() and flush() have nothing to say.
  File check = to.open(dst, FILE_READ);
  const bool complete = check && check.size() == expect;
  if (check) check.close();
  return complete;
}

static void removeTree(fs::FS& fs, const char* path) {
  const Entry e = entryAt(fs, path);
  if (!e.found) return;
  if (!e.isDir) { fs.remove(path); return; }
  for (const std::string& n : e.names) {
    char child[160];
    snprintf(child, sizeof(child), "%s/%s", path, n.c_str());
    breathe();
    removeTree(fs, child);
  }
  fs.rmdir(path);
}

static bool copyTree(fs::FS& from, const char* src, fs::FS& to, const char* dst) {
  const Entry e = entryAt(from, src);
  if (!e.found) return false;
  if (!e.isDir) { sCopied++; progress("file"); return copyFile(from, src, to, dst); }
  if (!to.mkdir(dst)) return false;
  for (const std::string& n : e.names) {
    char s[160], d[160];
    snprintf(s, sizeof(s), "%s/%s", src, n.c_str());
    snprintf(d, sizeof(d), "%s/%s", dst, n.c_str());
    breathe();
    if (!copyTree(from, s, to, d)) return false;
  }
  return true;
}

// The same shape and the same sizes at both ends. copyFile checks the bytes it
// moved; this checks that nothing was missed altogether — an entry the
// directory read stepped over, a file the destination quietly declined to
// create — which is the failure that leaves a store looking complete and one
// segment short. Nothing is deleted until this has agreed.
static bool treeMatches(fs::FS& a, const char* pa, fs::FS& b, const char* pb) {
  const Entry ea = entryAt(a, pa);
  const Entry eb = entryAt(b, pb);
  if (!ea.found || !eb.found || ea.isDir != eb.isDir) return false;
  if (!ea.isDir) return ea.size == eb.size;
  if (ea.names.size() != eb.names.size()) return false;
  for (const std::string& n : ea.names) {
    char s[160], d[160];
    snprintf(s, sizeof(s), "%s/%s", pa, n.c_str());
    snprintf(d, sizeof(d), "%s/%s", pb, n.c_str());
    breathe();
    if (!treeMatches(a, s, b, d)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
static bool remember(Move m) {
  TransportSettings t = settings.transport();
  t.pendingMove = m;
  return settings.saveTransport(t);
}

static bool queue(Move m, const char* said) {
  if (!remember(m)) { strlcpy(sResult, "failed: the request could not be saved", sizeof(sResult)); return false; }
  strlcpy(sResult, said, sizeof(sResult));
  log_w("store: %s", sResult);
  wifiManager.scheduleRestart(1200);
  return true;
}

bool requestAdopt() {
  const Ownership o = ownership();
  if (busy())                  { strlcpy(sResult, "a move is already queued", sizeof(sResult)); return false; }
  if (o.card == Card::NoCard)  { strlcpy(sResult, "no card", sizeof(sResult)); return false; }
  if (where() == Where::Sd)    { strlcpy(sResult, "the store is already on the card", sizeof(sResult)); return false; }
  if (o.card == Card::Foreign) {
    snprintf(sResult, sizeof(sResult),
             "refused: this card holds the store of \"%s\"; format it first if you mean to take it",
             o.owner[0] ? o.owner : "another node");
    return false;
  }
  return queue(Move::Adopt, "queued: the store moves onto the card as the node restarts");
}

bool requestEject() {
  if (busy())               { strlcpy(sResult, "a move is already queued", sizeof(sResult)); return false; }
  if (where() != Where::Sd) { strlcpy(sResult, "the store is not on the card", sizeof(sResult)); return false; }
  return queue(Move::Eject, "queued: the store moves to internal flash as the node restarts");
}

// ---------------------------------------------------------------------------
// The move itself, at boot, with nothing open
// ---------------------------------------------------------------------------
void runPendingMigration() {
  refreshCache();                        // what is actually in the slot this boot
  const TransportSettings before = settings.transport();
  const Move m = planAtBoot(before.pendingMove, before.sdStore, sdCard.mounted(), card());

  if (m == Move::None) {
    // Only report a refusal for a move somebody asked for. The card in the
    // slot at the restart need not be the card the request was made about, so
    // the request is dropped rather than kept waiting for a card that may
    // never come back.
    if (before.pendingMove != Move::None) {
      snprintf(sResult, sizeof(sResult), "failed: %s",
               !sdCard.mounted() ? "no card in the slot when the node restarted"
                                 : "this card is not carrying this node's store");
      log_e("store: %s", sResult);
      remember(Move::None);
    }
    return;
  }

  sRunning = true;
  const bool adopt = (m == Move::Adopt);
  fs::FS& from = adopt ? (fs::FS&)LittleFS : (fs::FS&)SD;
  fs::FS& to   = adopt ? (fs::FS&)SD       : (fs::FS&)LittleFS;
  log_w("store: moving %s -> %s", whereName(adopt ? Where::LittleFs : Where::Sd),
        whereName(adopt ? Where::Sd : Where::LittleFs));
  // Nothing else is alive yet to paint the panel, and this is about to take
  // several seconds. Say what is happening before it starts, not after.
  display.notice("Moving store", adopt ? "to the card" : "to flash");

  Marker mk;
  readMarker(mk);
  const uint32_t generation = mk.generation + 1;

  // A node that has never written a store has nothing to move: the tree is
  // created by the library the first time it is used. That is a move that is
  // already done, not a failure, and it is the ordinary case for a new node
  // with a card in the slot from the start.
  if (from.exists(RNS_FS_ROOT)) {
    // The copy is assembled beside the destination's own store and nothing is
    // deleted to make room for it. Removing the destination first and copying
    // into the hole is how a card pulled mid-copy, or a filesystem that ran
    // out of room, ended with a half-written store at one end and nothing at
    // the other.
    removeTree(to, kStagingRoot);
    if (!copyTree(from, RNS_FS_ROOT, to, kStagingRoot) ||
        !treeMatches(from, RNS_FS_ROOT, to, kStagingRoot)) {
      removeTree(to, kStagingRoot);
      display.notice("Store move", "failed");
    strlcpy(sResult, "failed: the store could not be copied, nothing was moved", sizeof(sResult));
      log_e("store: %s, staying on %s", sResult, whereName(where()));
      remember(Move::None);
      sRunning = false;
      return;
    }

    // From here the destination's old copy goes and the verified one takes its
    // place. The request stays on record across that window on purpose: a
    // power cut in it is retried from the source, which is still whole, at the
    // next boot.
    remember(m);
    removeTree(to, RNS_FS_ROOT);
    if (!to.rename(kStagingRoot, RNS_FS_ROOT) ||
        !treeMatches(from, RNS_FS_ROOT, to, RNS_FS_ROOT)) {
      display.notice("Store move", "failed");
    strlcpy(sResult, "failed: the copy could not be put in place, retrying at the next restart",
              sizeof(sResult));
      log_e("store: %s", sResult);
      sRunning = false;
      return;
    }
  }

  // The marker is written before the setting, so a power cut between them
  // leaves a card that names its owner and a node that has not moved yet —
  // recoverable — rather than a node pointed at a card that does not have the
  // data. An eject leaves the name on the card and marks it released, which is
  // what lets any node take it afterwards.
  writeMarker(generation, /*released=*/!adopt);

  // The setting is the point of no return: after it the node boots into the
  // new home, so it is written while both copies still exist. The old tree
  // goes afterwards. A power cut between the two leaves a stale tree behind —
  // cleared by the next move in that direction — and never a node pointed at a
  // home with nothing in it.
  TransportSettings t = settings.transport();
  t.sdStore     = adopt;
  t.pendingMove = Move::None;
  if (!settings.saveTransport(t)) {
    // This setting is what the next boot reads, so if it did not stick, the
    // node comes back up in the home it started in — and that home must still
    // have the data in it. The source stays, the request stays on record, and
    // the move is retried. Deleting it here on the strength of a write that
    // failed is the one path through this routine that loses the store
    // outright, which is worth a branch even though NVS rarely refuses.
    display.notice("Store move", "failed");
    strlcpy(sResult, "failed: the move could not be recorded, retrying at the next restart",
            sizeof(sResult));
    log_e("store: %s", sResult);
    sRunning = false;
    return;
  }

  // One home, not two, which is the whole claim this file makes.
  removeTree(from, RNS_FS_ROOT);
  refreshCache();

  display.notice("Store moved", adopt ? "to the card" : "to flash");
  snprintf(sResult, sizeof(sResult), "ok: store moved to %s",
           adopt ? "the card" : "internal flash");
  log_w("store: %s", sResult);
  sdCard.log(adopt ? "store: adopted this card" : "store: released this card");
  sRunning = false;
}

Where chooseAtBoot() {
  refreshCache();
  const Card c = card();
  const Where w = decide(settings.transport().sdStore, sdCard.mounted(), c);
  if (w == Where::Sd) {
    RnsFileSystem::useSd();
    sdCard.reserve(true);                // no formatting, and removal is an error
  } else {
    RnsFileSystem::useLittleFs();
  }
  // Keep the card's account of itself true.
  //
  // An unsigned card gets a name: only a store from before markers existed
  // reaches here, since a blank card is signed by the copy that makes it a
  // home, and an unsigned card is one some other node will read as free.
  //
  // A card this node already owns is still checked, because the name in the
  // marker is a copy of something the operator can change. Rename the node and
  // the card goes on claiming to belong to a name that no longer exists, which
  // defeats the one job that field has — telling a person holding the card
  // whose it is. Ownership itself never depended on the name: it is the node's
  // identity that is compared, and that is random bytes in NVS from first boot,
  // so renaming has never risked the node disowning its own store.
  //
  // The generation does not move for a rename. Nothing moved; a label was
  // wrong. And a name that already matches is not rewritten, so this is not a
  // flash write on every boot.
  if (w == Where::Sd && (c == Card::Legacy || c == Card::Ours)) {
    Marker m;
    const bool marked = readMarker(m);
    if (!marked || strcmp(m.name, wifiManager.hostname()) != 0) {
      if (writeMarker(marked ? m.generation : m.generation + 1, /*released=*/false))
        log_i("store: this card is marked as belonging to %s", wifiManager.hostname());
      refreshCache();
    }
  }
  return w;
}

#else   // !HAS_SD — no slot, so there is nothing to decide and nothing to move.

void      begin()               {}
Ownership ownership()           { return Ownership{}; }
Card      card()                { return Card::NoCard; }
bool      busy()                { return false; }
bool      requestAdopt()        { return false; }
bool      requestEject()        { return false; }
void      refreshCache()        {}
void      runPendingMigration() {}
Where     chooseAtBoot()        { RnsFileSystem::useLittleFs(); return Where::LittleFs; }

#endif

} // namespace StoreHome
