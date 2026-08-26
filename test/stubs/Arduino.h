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
