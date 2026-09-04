// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  Rtc.h — a clock that keeps running when the firmware is not
//
//  Without one, a node's only clock is the GNSS receiver, and time() counts
//  from the epoch until that receiver has a fix. Two minutes after boot the
//  node believes it is 1970-01-01 00:02, which is not a slightly wrong reading:
//  an LXMF message sent then carries that as its timestamp, and Sideband files
//  it below every real message its recipient has — sent, delivered, and buried.
//  RnsTransport already refuses to publish telemetry stamped before 2025 for
//  the same reason, which suppresses the symptom rather than the cause.
//
//  A part on the board that holds the time across a restart is the cause fixed:
//  the receiver seeds it once, and every boot afterwards starts with a clock
//  that is already right — indoors, with no sky, before the radio is up.
//
//  How much it is worth depends on what backs the part, which is a question
//  about the board rather than the chip. Backed by a cell or a supercap, it
//  survives being unplugged. Sitting on a rail that dies with the node, it
//  survives a reboot and nothing more. Both are an improvement on counting from
//  1970 and the code is the same either way, so nothing here needs to know
//  which it is; `lostPower()` reports what the part says about it.
// ============================================================================
#pragma once

#include "Config.h"

#if HAS_RTC

#include <stdint.h>
#include <time.h>

namespace Rtc {

// Probe the part and, if it is holding a time worth having, set the system
// clock from it. Call before anything can timestamp: the whole point is that
// time() is already right by the time the first message can be written.
void begin();
bool present();

// The part's own reading. False when it has none worth trusting — either it
// never answered, or it says its oscillator stopped since the time was set.
bool read(time_t& out);

// Record a time known good. The receiver's, in practice.
bool write(time_t t);

// Whether the part reports that it lost the time it was holding. Distinct from
// a part that is absent, and the two look identical from any distance: a node
// that boots with no clock either has no RTC or has one that went flat.
bool lostPower();

} // namespace Rtc

#else
namespace Rtc {
inline void begin() {}
inline bool present() { return false; }
inline bool read(time_t&) { return false; }
inline bool write(time_t) { return false; }
inline bool lostPower() { return false; }
} // namespace Rtc
#endif // HAS_RTC
