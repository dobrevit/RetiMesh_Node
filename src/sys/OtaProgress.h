// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.

// ============================================================================
//  OtaProgress.h — the installer's narrow progress feed
//
//  The installer reports bytes and phase words through these; on the device
//  OtaUpdate sinks them into the one Ota::Progress record the portal already
//  serves, and in host tests nothing registers and the calls vanish. The
//  previous version kept a second record here, and the glass and the portal
//  told two different stories about the same update — the glass a dead
//  screen through the whole receive, the portal a progress bar.
// ============================================================================
#pragma once

#include <stdint.h>

namespace OtaProgress {

struct Sink {
  void (*begin)(uint32_t total);
  void (*step)(uint32_t done);
  void (*phase)(const char* word);
};

inline Sink sSink{nullptr, nullptr, nullptr};

inline void setSink(const Sink& s)  { sSink = s; }
inline void begin(uint32_t total)   { if (sSink.begin) sSink.begin(total); }
inline void step(uint32_t done)     { if (sSink.step)  sSink.step(done); }
inline void phase(const char* w)    { if (sSink.phase) sSink.phase(w); }
// Outcomes are the caller's stage writes (OtaUpdate's say()); the installer's
// scope guard still calls this so the call sites need not know.
inline void end(bool) {}

} // namespace OtaProgress
