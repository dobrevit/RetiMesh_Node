// Minimal Arduino.h stand-in so pure headers (HDLC.h, Config.h) compile in
// the host-native unit-test environment.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
using std::min;
using std::max;
struct IPAddress { IPAddress(int, int, int, int) {} };

// strlcpy is BSD's and the ESP32 core's, not the C library's; the rules and
// the settings structs use it, so the host build needs one that behaves the
// same way — truncate, always terminate, return the length it wanted.
inline size_t strlcpy(char* dst, const char* src, size_t size) {
  const size_t len = std::strlen(src);
  if (size) {
    const size_t n = len < size - 1 ? len : size - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}
