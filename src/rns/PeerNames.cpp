// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// PeerNames.cpp — see PeerNames.h.
#include "PeerNames.h"

#if HAS_LVGL_UI

#include <string.h>
#include <stdio.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>

static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;

namespace PeerNames {
namespace {

struct Entry { char hash[33]; char name[33]; };
constexpr size_t kMax = 48;
Entry  sCache[kMax];
size_t sCount = 0;
size_t sNext = 0;                        // overwrite the oldest once full
bool   sLoaded = false;
constexpr const char* kFile = "/peer_names.txt";

// The file is parsed into a scratch table with no lock held — LittleFS is
// slow and a spinlock must never wait on it — and installed under the lock
// in one motion, sLoaded last. Two tasks racing here both parse; one
// installs; the other's work is discarded. The first draft set sLoaded
// first and filled the live table unlocked, and a lookup mid-parse read a
// half-written name.
void loadIfNeeded() {
  {
    taskENTER_CRITICAL(&sMux);
    const bool done = sLoaded;
    taskEXIT_CRITICAL(&sMux);
    if (done) return;
  }
  Entry scratch[kMax];
  size_t n = 0;
  File f = LittleFS.open(kFile, "r");
  if (f) {
    char line[80];
    while (f.available() && n < kMax) {
      const size_t got = f.readBytesUntil('\n', (uint8_t*)line, sizeof(line) - 1);
      line[got] = 0;
      // Older files were written with println and carry a carriage return.
      if (got && line[got - 1] == '\r') line[got - 1] = 0;
      char* sp = strchr(line, ' ');
      if (!sp || sp - line != 32) continue;
      *sp = 0;
      strlcpy(scratch[n].hash, line, sizeof(scratch[n].hash));
      strlcpy(scratch[n].name, sp + 1, sizeof(scratch[n].name));
      if (scratch[n].name[0]) n++;
    }
    f.close();
  }
  taskENTER_CRITICAL(&sMux);
  if (!sLoaded) {
    memcpy(sCache, scratch, sizeof(Entry) * n);
    sCount = n;
    sNext = n % kMax;
    sLoaded = true;
  }
  taskEXIT_CRITICAL(&sMux);
}

void save() {
  File f = LittleFS.open(kFile, "w");
  if (!f) return;
  taskENTER_CRITICAL(&sMux);
  Entry copy[kMax];
  const size_t n = sCount;
  memcpy(copy, sCache, sizeof(Entry) * n);
  taskEXIT_CRITICAL(&sMux);
  for (size_t i = 0; i < n; i++) {
    f.print(copy[i].hash);
    f.print(' ');
    f.print(copy[i].name);
    f.print('\n');                       // never println: \r poisoned the read-back
  }
  f.close();
}

} // namespace

void remember(const char* hashHex, const char* name) {
  if (!hashHex || !name || !hashHex[0] || !name[0] || strlen(hashHex) != 32) return;
  loadIfNeeded();
  bool changed = false;
  taskENTER_CRITICAL(&sMux);
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
  taskEXIT_CRITICAL(&sMux);
  if (changed) save();                   // outside the lock; the file is slow
}

bool lookup(const char* hashHex, char* out, size_t n) {
  if (!hashHex || strlen(hashHex) < 32) return false;
  loadIfNeeded();
  bool found = false;
  taskENTER_CRITICAL(&sMux);
  for (size_t i = 0; i < sCount; i++)
    if (strncmp(sCache[i].hash, hashHex, 32) == 0) {
      strlcpy(out, sCache[i].name, n);
      found = true;
      break;
    }
  taskEXIT_CRITICAL(&sMux);
  return found;
}

void wipe() {
  taskENTER_CRITICAL(&sMux);
  sCount = 0;
  sNext = 0;
  sLoaded = true;                        // an emptied table is a loaded table
  taskEXIT_CRITICAL(&sMux);
  LittleFS.remove(kFile);
}

} // namespace PeerNames
#endif // HAS_LVGL_UI
