// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See StoreHome.h.

#include "StoreHome.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include "RnsFileSystem.h"
#include "Settings.h"
#if HAS_SD
  #include <SD.h>
  #include "SdCard.h"
#endif
#include "RnsAnnounce.h"
#include "WifiManager.h"

namespace StoreHome {

// The marker sits beside the event log rather than inside the store, so that
// wiping the store does not silently un-own the card, and so a person putting
// the card in a laptop finds both files in one obvious place.
static const char* kMarkerPath = "/retimesh/store.json";
static const char* kMarkerDir  = "/retimesh";
static const uint8_t kSchema   = 1;

static Where sWhere = Where::LittleFs;
static char  sResult[64] = "";
static volatile uint8_t sRequest = 0;      // 0 none, 1 adopt, 2 eject

// ---------------------------------------------------------------------------
// The rule
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

Where where()       { return sWhere; }
const char* lastResult() { return sResult; }
bool busy()         { return sRequest != 0; }

// ---------------------------------------------------------------------------
// The marker
// ---------------------------------------------------------------------------
#if HAS_SD

bool readMarker(Marker& out) {
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

Card card() {
  if (!sdCard.mounted()) return Card::NoCard;
  Marker m;
  if (readMarker(m))
    return strcmp(m.node, nodeIdentity.identityHex()) == 0 ? Card::Ours : Card::Foreign;
  // No marker. A store already on the card was put there by this node before
  // markers existed — a card does not acquire one by accident — so it is ours.
  return SD.exists(RNS_FS_ROOT) ? Card::Legacy : Card::Blank;
}

// ---------------------------------------------------------------------------
// Moving the files
//
// Both ends are fs::FS, so one routine serves either direction. The store is
// small — index and a few segments per table — so this is plain reads and
// writes with no attempt at being clever about it.
// ---------------------------------------------------------------------------
static uint8_t sBuf[512];

static bool copyFile(fs::FS& from, const char* src, fs::FS& to, const char* dst) {
  File in = from.open(src, FILE_READ);
  if (!in) return false;
  File out = to.open(dst, FILE_WRITE);
  if (!out) { in.close(); return false; }
  bool ok = true;
  while (ok) {
    const size_t n = in.read(sBuf, sizeof(sBuf));
    if (n == 0) break;
    ok = out.write(sBuf, n) == n;
  }
  in.close();
  out.close();
  return ok;
}

static void removeTree(fs::FS& fs, const char* path) {
  File dir = fs.open(path);
  if (!dir) return;
  if (!dir.isDirectory()) { dir.close(); fs.remove(path); return; }
  File e;
  while ((e = dir.openNextFile())) {
    char child[128];
    snprintf(child, sizeof(child), "%s/%s", path, e.name());
    const bool isDir = e.isDirectory();
    e.close();
    if (isDir) removeTree(fs, child);
    else       fs.remove(child);
  }
  dir.close();
  fs.rmdir(path);
}

static bool copyTree(fs::FS& from, const char* src, fs::FS& to, const char* dst) {
  File dir = from.open(src);
  if (!dir) return false;
  if (!dir.isDirectory()) { dir.close(); return copyFile(from, src, to, dst); }
  to.mkdir(dst);
  bool ok = true;
  File e;
  while (ok && (e = dir.openNextFile())) {
    char s[128], d[128];
    snprintf(s, sizeof(s), "%s/%s", src, e.name());
    snprintf(d, sizeof(d), "%s/%s", dst, e.name());
    const bool isDir = e.isDirectory();
    e.close();
    ok = isDir ? copyTree(from, s, to, d) : copyFile(from, s, to, d);
  }
  dir.close();
  return ok;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
bool requestAdopt() {
  if (sRequest) return false;
  if (!sdCard.mounted())        { strlcpy(sResult, "no card", sizeof(sResult)); return false; }
  if (sWhere == Where::Sd)      { strlcpy(sResult, "already on the card", sizeof(sResult)); return false; }
  if (card() == Card::Foreign)  { strlcpy(sResult, "refused: another node's store", sizeof(sResult)); return false; }
  sRequest = 1;
  return true;
}

bool requestEject() {
  if (sRequest) return false;
  if (sWhere != Where::Sd)      { strlcpy(sResult, "the store is not on the card", sizeof(sResult)); return false; }
  sRequest = 2;
  return true;
}

// Called from the RNS task, the only task that writes the store, so nothing is
// being written while the files move.
void service() {
  const uint8_t req = sRequest;
  if (!req) return;

  const bool adopt = (req == 1);
  fs::FS& from = adopt ? (fs::FS&)LittleFS : (fs::FS&)SD;
  fs::FS& to   = adopt ? (fs::FS&)SD       : (fs::FS&)LittleFS;

  Marker m;
  readMarker(m);
  const uint32_t generation = m.generation + 1;

  log_w("store: moving %s -> %s", adopt ? "littlefs" : "sd", adopt ? "sd" : "littlefs");
  removeTree(to, RNS_FS_ROOT);
  const bool copied = copyTree(from, RNS_FS_ROOT, to, RNS_FS_ROOT);

  if (!copied) {
    // The destination is now a partial copy, which is why nothing has been
    // pointed at it yet: the node restarts into the home it already had and
    // the half-written tree is overwritten by the next attempt.
    strlcpy(sResult, "failed: copy error", sizeof(sResult));
    log_e("store: copy failed, staying on %s", whereName(sWhere));
    sRequest = 0;
    return;
  }

  // The marker is written after the copy and before the setting, so a power
  // cut in the middle leaves a card that names its owner and a node that has
  // not yet moved — recoverable — rather than a node pointed at a card that
  // does not have the data.
  writeMarker(generation, /*released=*/!adopt);

  TransportSettings t = settings.transport();
  t.sdStore = adopt;
  settings.saveTransport(t);

  snprintf(sResult, sizeof(sResult), "ok: store moved to %s, restarting",
           adopt ? "the card" : "internal flash");
  log_w("store: %s", sResult);
  sdCard.log(adopt ? "store: adopted this card" : "store: released this card");
  sRequest = 0;
  wifiManager.scheduleRestart(1200);
}

Where chooseAtBoot() {
  const Card c = card();
  sWhere = decide(settings.transport().sdStore, sdCard.mounted(), c);
  // Put this node's name on a card it is taking into use but has not signed.
  // Either it predates markers, or the setting was on when a blank card went
  // in; both end with the store on a card that does not say whose it is, and
  // an unsigned card is one that some other node will read as free.
  if (sWhere == Where::Sd && (c == Card::Legacy || c == Card::Blank)) {
    Marker m;
    readMarker(m);
    if (writeMarker(m.generation + 1, /*released=*/false))
      log_i("store: this card is now marked as belonging to %s", wifiManager.hostname());
  }
  return sWhere;
}

#else   // !HAS_SD — no slot, so there is nothing to decide and nothing to move.

bool readMarker(Marker& out) { out = Marker{}; return false; }
Card card()                  { return Card::NoCard; }
bool requestAdopt()          { return false; }
bool requestEject()          { return false; }
void service()               {}
Where chooseAtBoot()         { sWhere = Where::LittleFs; return sWhere; }

#endif

} // namespace StoreHome
