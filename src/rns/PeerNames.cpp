// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// PeerNames.cpp — see PeerNames.h.
#include "PeerNames.h"

#include <string.h>
#include <stdio.h>

#if __has_include(<LittleFS.h>)
#include <LittleFS.h>
#define PEER_NAMES_FS 1
#else
#define PEER_NAMES_FS 0                  // host tests: RAM only, same behavior
#endif

#if __has_include(<freertos/FreeRTOS.h>)
#include <freertos/FreeRTOS.h>
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   taskENTER_CRITICAL(&sMux)
#define UNLOCK() taskEXIT_CRITICAL(&sMux)
#else
#define LOCK()
#define UNLOCK()
#endif

namespace PeerNames {
namespace {

struct Entry { char hash[33]; char name[33]; };
constexpr size_t kMax = 48;
Entry  sCache[kMax];
size_t sCount = 0;
size_t sNext = 0;                        // overwrite the oldest once full
bool   sLoaded = false;
constexpr const char* kFile = "/peer_names.txt";

void load() {
  if (sLoaded) return;
  sLoaded = true;
#if PEER_NAMES_FS
  File f = LittleFS.open(kFile, "r");
  if (!f) return;
  char line[80];
  size_t w = 0;
  while (f.available() && sCount < kMax) {
    const size_t n = f.readBytesUntil('\n', (uint8_t*)line, sizeof(line) - 1);
    line[n] = 0;
    char* sp = strchr(line, ' ');
    if (!sp || sp - line != 32) continue;
    *sp = 0;
    Entry& e = sCache[sCount++];
    strlcpy(e.hash, line, sizeof(e.hash));
    strlcpy(e.name, sp + 1, sizeof(e.name));
    (void)w;
  }
  f.close();
  sNext = sCount % kMax;
#endif
}

void save() {
#if PEER_NAMES_FS
  File f = LittleFS.open(kFile, "w");
  if (!f) return;
  for (size_t i = 0; i < sCount; i++) {
    f.print(sCache[i].hash);
    f.print(' ');
    f.println(sCache[i].name);
  }
  f.close();
#endif
}

} // namespace

void remember(const char* hashHex, const char* name) {
  if (!hashHex || !name || !hashHex[0] || !name[0] || strlen(hashHex) != 32) return;
  load();
  bool changed = false;
  LOCK();
  size_t i = 0;
  for (; i < sCount; i++)
    if (strcmp(sCache[i].hash, hashHex) == 0) break;
  if (i == sCount) {
    Entry& e = sCache[sCount < kMax ? sCount : sNext];
    if (sCount < kMax) sCount++; else sNext = (sNext + 1) % kMax;
    strlcpy(e.hash, hashHex, sizeof(e.hash));
    strlcpy(e.name, name, sizeof(e.name));
    changed = true;
  } else if (strcmp(sCache[i].name, name) != 0) {
    strlcpy(sCache[i].name, name, sizeof(sCache[i].name));
    changed = true;
  }
  UNLOCK();
  // Written outside the spinlock: the file is slow and the table is not.
  if (changed) save();
}

bool lookup(const char* hashHex, char* out, size_t n) {
  if (!hashHex || strlen(hashHex) < 32) return false;
  load();
  bool found = false;
  LOCK();
  for (size_t i = 0; i < sCount; i++)
    if (strncmp(sCache[i].hash, hashHex, 32) == 0) {
      strlcpy(out, sCache[i].name, n);
      found = true;
      break;
    }
  UNLOCK();
  return found;
}

} // namespace PeerNames
