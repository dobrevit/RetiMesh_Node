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
  #include <sys/stat.h>
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
// Absent and unreadable are different answers and the caller acts on the
// difference (see classify): no marker means nobody has claimed this card,
// while a marker that will not parse means somebody has and this node cannot
// check the claim. The file is written with FILE_WRITE, which truncates it
// before anything is serialised in, so a power cut mid-write leaves exactly the
// zero-byte case this has to tell apart from a card with no marker on it.
static MarkerState readMarker(Marker& out) {
  out = Marker{};
  if (!sdCard.mounted()) return MarkerState::Absent;
  File f = SD.open(kMarkerPath, FILE_READ);
  // The extra existence check costs an open, and only on the path where the
  // first one already failed: a file that is there and will not open is a card
  // refusing to answer, not a card with nothing to say.
  if (!f) return SD.exists(kMarkerPath) ? MarkerState::Unreadable : MarkerState::Absent;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return MarkerState::Unreadable;
  strlcpy(out.node, doc["node"] | "", sizeof(out.node));
  strlcpy(out.name, doc["name"] | "", sizeof(out.name));
  out.generation = doc["generation"] | 0;
  out.released   = doc["released"] | false;
  out.valid      = out.node[0] != '\0';
  // Valid JSON with no identity in it names nobody, so it cannot be checked
  // either. Same answer as a file that would not parse at all.
  return out.valid ? MarkerState::Read : MarkerState::Unreadable;
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

// Whether the card holds a store, without opening anything: SD.exists() opens
// the path read-only and logs an error when it is missing, which is a file open
// and a log line to answer a question stat() answers in silence.
// RnsFileSystem::present() does the same trick for the store's own backend and
// cannot be borrowed here, because it asks whichever filesystem the store is
// pointed at and the question is specifically about the card.
static bool cardHas(const char* path) {
  char full[80];
  snprintf(full, sizeof(full), "%s%s", SdCard::MOUNT_POINT, path);
  struct stat st;
  return stat(full, &st) == 0;
}

// Read on the card task, handed out from memory everywhere else. Called when
// something can have changed it — a card arriving or leaving, a format, a move,
// a marker rewritten — and not on a timer: it opens and parses a file and
// allocates a JsonDocument, and doing that every three seconds for the life of
// the node came to some thirty thousand opens a day on the bus the store is
// being written to, to notice a file that changes when this node changes it.
void refreshCache() {
  if (!sCacheLock) return;
  Ownership o;
  if (sdCard.mounted()) {
    Marker m;
    const MarkerState marker = readMarker(m);
    o.card = classify(true, marker,
                      marker == MarkerState::Read &&
                        strcmp(m.node, nodeIdentity.identityHex()) == 0,
                      m.released, cardHas(RNS_FS_ROOT));
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
static uint32_t sLastPaintMs = 0;
static const uint32_t kProgressPaintMs = 250;   // four frames a second

static void progress(const char* what) {
  // Painting is not free: notice() clears the buffer and pushes the whole
  // kilobyte of it over I2C, some 25 ms at 400 kHz, so a store of a few hundred
  // files spent longer drawing the count than copying the bytes — on the boot
  // path, with the node not yet up. Four frames a second reads as "still
  // working" exactly as well as every frame does. The count itself keeps rising
  // per file; this only stops reporting every single step of it.
  const uint32_t now = millis();
  if (now - sLastPaintMs < kProgressPaintMs) return;
  sLastPaintMs = now;
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

// The facts each rule is applied to, gathered in one place so that the button
// the page draws and the request it sends are answering the same question. A
// second reading of them, assembled in the status handler, is how the Eject
// button came to be lit for a card that was no longer in the slot.
static const char* adoptRefusalNow() {
  return adoptRefusal(busy(), where(), card(), sdCard.storageLost());
}
static const char* ejectRefusalNow() {
  return ejectRefusal(busy(), where(), sdCard.storageLost());
}

bool canAdopt() { return adoptRefusalNow() == nullptr; }
bool canEject() { return ejectRefusalNow() == nullptr; }

bool requestAdopt() {
  const char* why = adoptRefusalNow();
  if (why) {
    // The rule's sentence, finished with the name on the card where there is
    // one: whose store this is cannot be seen by a pure rule, and is the first
    // thing the operator wants to know.
    const Ownership o = ownership();
    if (o.card == Card::Foreign && o.owner[0])
      snprintf(sResult, sizeof(sResult),
               "refused: this card holds the store of \"%s\"; format it first if you mean to take it",
               o.owner);
    else
      strlcpy(sResult, why, sizeof(sResult));
    return false;
  }
  return queue(Move::Adopt, "queued: the store moves onto the card as the node restarts");
}

bool requestEject() {
  const char* why = ejectRefusalNow();
  if (why) { strlcpy(sResult, why, sizeof(sResult)); return false; }
  return queue(Move::Eject, "queued: the store moves to internal flash as the node restarts");
}

// ---------------------------------------------------------------------------
// The move itself, at boot, with nothing open
// ---------------------------------------------------------------------------
// How a move that did not work ends. Three things have to happen together and
// used to happen separately: the operator is told, the request is taken off the
// books, and the "a move is in progress" state is cleared.
//
// Taking the request off the books is the part that changed. A failed move used
// to leave it queued and promise a retry at the next restart — a restart
// nothing scheduled. Until somebody power-cycled the node, every question that
// asks whether a move is under way answered yes: the format was refused, both
// buttons on the settings page stayed grey, and the page went on saying the
// store was about to move. Nothing was lost by any of the failure paths — each
// one leaves the store readable where it already was — so the honest ending is
// to say what happened and let the operator ask again, on a node that is
// otherwise working normally.
static void fail(const char* what) {
  display.notice("Store move", "failed");
  strlcpy(sResult, what, sizeof(sResult));
  // Which home the node is about to open, asked of the rule rather than of
  // where(): nothing has pointed the store's filesystem anywhere yet at this
  // point in the boot, so where() answers with its default and this line used
  // to say "littlefs" about a store sitting on the card.
  const TransportSettings t = settings.transport();
  log_e("store: %s, staying on %s", sResult,
        whereName(decide(t.sdStore, sdCard.mounted(), card())));
  remember(Move::None);
  sRunning = false;
}

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
      char why[128];
      snprintf(why, sizeof(why), "failed: %s",
               !sdCard.mounted() ? "no card in the slot when the node restarted"
                                 : "this card is not carrying this node's store");
      fail(why);
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

  // The copy is assembled beside the destination's own store and nothing is
  // deleted to make room for it. Removing the destination first and copying
  // into the hole is how a card pulled mid-copy, or a filesystem that ran out
  // of room, ended with a half-written store at one end and nothing at the
  // other. Anything an interrupted attempt left behind goes now.
  removeTree(to, kStagingRoot);

  // A node that has never written a store has nothing to copy: the tree is
  // created by the library the first time it is used. That is a move that is
  // already done, not a failure, and it is the ordinary case for a new node
  // with a card in the slot from the start.
  const bool haveSource = from.exists(RNS_FS_ROOT);
  if (haveSource) {
    if (!copyTree(from, RNS_FS_ROOT, to, kStagingRoot) ||
        !treeMatches(from, RNS_FS_ROOT, to, kStagingRoot)) {
      removeTree(to, kStagingRoot);
      fail("failed: the store could not be copied, nothing was moved");
      return;
    }
    // From here the destination's old copy goes and the verified one takes its
    // place. The request stays on record across that window on purpose: a power
    // cut in it is retried from the source, which is still whole, at the next
    // boot.
    remember(m);
  }

  // The destination's old tree goes whether or not there was anything to copy
  // onto it. Skipping this with the rest of the copy left a tree from some
  // earlier life sitting at the new home, and the node opened it: a store
  // nobody meant to keep, quietly promoted to the live one, with no sign that
  // anything had happened.
  removeTree(to, RNS_FS_ROOT);

  if (haveSource && (!to.rename(kStagingRoot, RNS_FS_ROOT) ||
                     !treeMatches(from, RNS_FS_ROOT, to, RNS_FS_ROOT))) {
    fail("failed: the copy could not be put in place; the store still works where it was");
    return;
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
    // have the data in it. The source is therefore left alone. Deleting it here
    // on the strength of a write that failed is the one path through this
    // routine that loses the store outright, which is worth a branch even
    // though NVS rarely refuses.
    fail("failed: the move could not be recorded; the store still works where it was");
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
    const bool marked = readMarker(m) == MarkerState::Read;
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
bool      canAdopt()            { return false; }
bool      canEject()            { return false; }
bool      requestAdopt()        { return false; }
bool      requestEject()        { return false; }
void      refreshCache()        {}
void      runPendingMigration() {}
Where     chooseAtBoot()        { RnsFileSystem::useLittleFs(); return Where::LittleFs; }

#endif

} // namespace StoreHome
