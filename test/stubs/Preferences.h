// Minimal Preferences stand-in for the host-native unit tests. Settings.h
// declares one as a private member, so the class has to be complete for the
// header to compile; nothing native calls it, because the tests exercise the
// pure rules and never the store.
#pragma once
#include <cstdint>
#include <cstddef>

class Preferences {
public:
  bool begin(const char*, bool = false) { return false; }
  void end() {}
  bool clear() { return false; }
  bool remove(const char*) { return false; }
  size_t putBytes(const char*, const void*, size_t) { return 0; }
  size_t getBytes(const char*, void*, size_t) { return 0; }
  size_t putString(const char*, const char*) { return 0; }
  size_t getString(const char*, char*, size_t) { return 0; }
  bool   putBool(const char*, bool) { return false; }
  bool   getBool(const char*, bool d = false) { return d; }
  uint32_t putUInt(const char*, uint32_t) { return 0; }
  uint32_t getUInt(const char*, uint32_t d = 0) { return d; }
  bool   isKey(const char*) { return false; }
};
