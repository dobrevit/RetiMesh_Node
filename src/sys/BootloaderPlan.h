// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

// ============================================================================
//  BootloaderPlan.h — how this board gets into its firmware download mode
//
//  Getting a running node back into the ROM downloader is the one thing every
//  flashing workflow needs and every board does differently. A USB-UART
//  bridge pulls EN and IO0 for esptool; an ESP32-S3 can ask its own ROM to
//  boot into download mode on the next reset; a native-USB CDC port can be
//  "touched" at 1200 baud; and when everything else has failed there is a
//  BOOT button and a human. Which of those apply is a fact about the silicon
//  and the PCB, and the rules that turn those facts into a plan live here,
//  where they can be tested without a board.
//
//  Two pieces, both pure:
//    plan()         which methods a board offers, best first
//    Sequencer      the request -> quiesce -> restart steps, with the delay
//                   that lets an acknowledgement leave before the link dies
//
//  The 1200-baud CDC touch is deliberately not here. It needs a CDC-ACM port
//  the firmware owns, so it can see the line coding, and no board runs one:
//  the S3 sits on its fixed USB-Serial/JTAG personality, which does the
//  DTR/RTS handshake in hardware and shows the firmware nothing. A detector
//  with no caller was sixty lines of header and seven tests exercising a path
//  the node could not take, and a method the API listed that nothing could
//  perform. It comes back with the composite device that can feed it.
//
//  The mechanism the software path relies on is documented rather than
//  guessed: ESP32-S2/S3/C3 carry RTC_CNTL_FORCE_DOWNLOAD_BOOT in
//  RTC_CNTL_OPTION1_REG, and writing it in a shutdown handler before
//  esp_restart() is what the Arduino core's usb_persist_restart() does for
//  its own 1200-baud touch — on the OTG controller. The classic ESP32 has no
//  such bit, so a running node there cannot put itself into the downloader;
//  only the bridge or a finger can. Bootloader.cpp is where the register is
//  actually written.
//
//  And a board whose console is the S3's own USB-Serial/JTAG unit cannot use
//  it either, which was learned on a T3-S3 rather than read anywhere: that
//  unit is not reset by the software reset esp_restart() performs, so the
//  host keeps its old enumeration while the ROM downloader comes up behind
//  it expecting a fresh one. The chip sits there hung — no console, no
//  downloader, no port drop — and not even EN recovers it: only removing
//  power did, on the bench, which says the state lives in a domain the
//  reset button does not reach. The unit does the
//  DTR/RTS handshake in hardware, which esptool performs unaided and which
//  works, so that is the method those boards offer. The software entry is
//  kept for the S3 behind a UART bridge, where the downloader talks on UART0
//  and the bridge is untouched by the chip resetting.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <atomic>
#if defined(ESP_PLATFORM)
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
#endif

namespace Bootloader {

enum class Method : uint8_t {
  SoftwareApi,      // firmware sets FORCE_DOWNLOAD_BOOT and restarts (HTTP or console)
  AutoResetDtrRts,  // esptool drives EN/IO0 through the bridge or USB-Serial/JTAG
  ManualRecovery,   // hold BOOT, press RESET
};

inline const char* methodName(Method m) {
  switch (m) {
    case Method::SoftwareApi:     return "software_api";
    case Method::AutoResetDtrRts: return "auto_reset_dtr_rts";
    case Method::ManualRecovery:  return "manual_recovery";
  }
  return "unknown";
}

// What the board can do. Filled from the silicon (compile-time) and the board
// capability flags (boards.json, via tools/board_caps.py).
struct Caps {
  bool forceDownloadBoot = false;   // silicon has RTC_CNTL_FORCE_DOWNLOAD_BOOT (S2/S3/C3)
  bool nativeUsb         = false;   // the chip's own USB is on the connector
  bool otgStack          = false;   // ...and the OTG stack owns it (the composite device)
  bool bridgeAutoReset   = false;   // a USB-UART bridge with DTR/RTS wired to EN/IO0
};

struct Plan {
  Method methods[3];
  size_t count = 0;
  bool has(Method m) const {
    for (size_t i = 0; i < count; i++) if (methods[i] == m) return true;
    return false;
  }
  Method primary() const { return methods[0]; }

  // The names, comma-joined, for the console and any other single-line
  // reader. One place, because the console had grown its own strlcat loop
  // beside the three JSON writers.
  const char* names(char* out, size_t len) const {
    if (len) out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
      const size_t used = strlen(out);
      if (used >= len) break;
      snprintf(out + used, len - used, "%s%s", i ? "," : "", methodName(methods[i]));
    }
    return out;
  }
};

// Whether firmware can get the chip into the downloader on its own, which is
// what the HTTP endpoint and the console command promise. Silicon with the
// bit, and a USB unit that will not be left hanging: the fixed serial-JTAG
// unit survives the software reset and the ROM cannot bring it back up
// until the power is cycled, but with the OTG stack in charge the core
// hands the peripheral back to the serial-JTAG unit before it restarts, and
// the ROM's downloader enumerates on it as usual — see the header.
inline bool canEnterAutomatically(const Caps& c) {
  return c.forceDownloadBoot && (!c.nativeUsb || c.otgStack);
}

// Why not, in the operator's words, from the same facts the decision used.
// An earlier version derived the text from a board macro one line after the
// decision had been made from Caps, and the two could name different causes.
inline const char* whyNotAutomatic(const Caps& c) {
  if (!c.forceDownloadBoot)
    return "this chip cannot enter its downloader from software; use the bridge's auto-reset or hold BOOT";
  if (c.nativeUsb && !c.otgStack)
    return "software entry hangs a native-USB board until power is cycled (its USB unit survives the reset); esptool's DTR/RTS handshake does it instead";
  return "";
}

// Every way a request can be turned down, and the HTTP status each maps to.
// One mapping, because the console and the HTTP handler each used to work
// the status out from a side query, in opposite polarity, and a third reason
// fell into whichever bucket the ternary happened to land it in.
enum class Refusal : uint8_t { None = 0, CannotEnter, CannotArm, Busy };

inline int httpStatus(Refusal r) {
  switch (r) {
    case Refusal::None:        return 202;   // accepted: the restart is armed
    case Refusal::CannotEnter: return 501;   // this board cannot, ever
    case Refusal::CannotArm:   return 500;   // this board could, and something in the system stopped it
    case Refusal::Busy:        return 409;   // a restart is already in progress
  }
  return 500;
}

// Best first. The API first because it works over any local link; the bridge
// reset next because esptool does it on its own; and recovery is always
// there, last, because it always works.
inline Plan plan(const Caps& c) {
  Plan p;
  if (canEnterAutomatically(c))        p.methods[p.count++] = Method::SoftwareApi;
  // The S3's USB-Serial/JTAG implements the same DTR/RTS handshake in
  // hardware, so native USB counts as a bridge here.
  if (c.bridgeAutoReset || c.nativeUsb) p.methods[p.count++] = Method::AutoResetDtrRts;
  p.methods[p.count++] = Method::ManualRecovery;
  return p;
}


// ---------------------------------------------------------------------------
// The request sequence
// ---------------------------------------------------------------------------
// Whether a request that did not arrive over the cable may ask for the ROM
// downloader. Two switches: whether the API answers at all, and — unless the
// caller is on a link the node is the host end of (the access point, usb0,
// ppp0) — whether the station network is allowed to ask.
//
// Here rather than in the HTTP handler because the console answers on a
// socket too, and a command that drops a relay into its ROM and leaves it
// there must not be easier to reach over one transport than the other. The
// cable does not come through here at all: physical access already means
// dumping the firmware and reflashing it, which is strictly more than this.
// *whyNot receives the words for a refusal, and the status for it is 403.
inline bool remoteEntryAllowed(bool hostFacing, bool apiEnabled, bool fromLanAllowed,
                               const char** whyNot = nullptr) {
  if (whyNot) *whyNot = "";
  if (!apiEnabled) {
    if (whyNot) *whyNot = "the bootloader API is switched off in maintenance settings";
    return false;
  }
  if (!hostFacing && !fromLanAllowed) {
    if (whyNot) *whyNot = "only from a directly attached link (access point, USB, PPP); "
                          "set bootloader_from_lan to allow it from the station network";
    return false;
  }
  return true;
}

enum class Target : uint8_t { App = 0, Bootloader = 1 };
enum class Source : uint8_t { Http = 0, Console, Settings, Touch, Ui };   // Touch: the 1200-baud touch on the USB console port; Ui: the glass

inline const char* targetName(Target t) { return t == Target::Bootloader ? "bootloader" : "app"; }
inline const char* sourceName(Source s) {
  switch (s) {
    case Source::Http:     return "http";
    case Source::Console:  return "console";
    case Source::Settings: return "settings";
    case Source::Touch:    return "touch";
    case Source::Ui:       return "ui";
  }
  return "unknown";
}

enum class State : uint8_t { Idle = 0, Armed, Quiescing, Restarting };

// What is armed, for whoever reports it: the console's STATUS, /api/status
// and the tests read the same fields the operator sees. Copied out under
// one lock, because three separate accessors could be read across a
// request() and describe one request's deadline with another's target.
struct Pending {
  State    state  = State::Idle;
  Target   target = Target::App;
  Source   source = Source::Http;
  uint32_t dueMs  = 0;
  bool armed() const { return state != State::Idle; }
  // Milliseconds until the deadline; 0 once it has passed or nothing is armed.
  uint32_t dueInMs(uint32_t nowMs) const {
    return armed() && (int32_t)(dueMs - nowMs) > 0 ? dueMs - nowMs : 0;
  }
};

inline const char* stateName(State s) {
  switch (s) {
    case State::Idle:       return "idle";
    case State::Armed:      return "armed";
    case State::Quiescing:  return "quiescing";
    case State::Restarting: return "restarting";
  }
  return "unknown";
}

// What tick() asks the caller to do this pass.
enum class Step : uint8_t { None = 0, Quiesce, Restart };

// The sequence is: a request arms a restart some milliseconds ahead — long
// enough for the HTTP reply or the console acknowledgement to leave the
// network stack. When the deadline passes the caller is told to quiesce
// (stop taking work, flush, unmount), and on the very next tick to restart.
// Two steps rather than one so the quiesce runs on the same task and
// context every time, whoever asked.
//
// A request for the bootloader outranks one for a plain reboot that is
// already armed: the operator who asked for the loader means it. The other
// way round is refused — a reboot request cannot quietly downgrade a pending
// bootloader entry that a flashing tool is already waiting on.
//
// request() runs on whichever task took the HTTP request or the console
// line; tick() runs on the loop task; and two requests can race each other
// from two tasks. The fields are therefore guarded by a lock rather than
// by a comment about store ordering, which is what an earlier version had,
// and which the compiler was under no obligation to honour: had it sunk the
// deadline store below the state store, a tick landing between them would
// have seen Armed with last time's deadline and restarted at once, before the
// acknowledgement had left. The critical sections are a few instructions.
class Sequencer {
public:
  // The least a restart may be from any accepted request: one pass of the
  // loop that sends the acknowledgement, with margin.
  static constexpr uint32_t kAckFloorMs = 250;

  bool request(Target target, Source source, uint32_t delayMs, uint32_t nowMs) {
    Guard g(_lock);
    const State st = _state.load(std::memory_order_relaxed);
    if (st == State::Quiescing || st == State::Restarting) return false;
    if (st == State::Armed && _target == Target::Bootloader && target == Target::App) return false;
    uint32_t due = nowMs + delayMs;
    // A second request while one is armed keeps the earlier of the two
    // deadlines. Two callers were each told their own delay; honouring the
    // later one would push the restart past what the first was promised, and
    // a page that re-posts on every retry would keep a reboot from ever
    // firing at all. But never so early that this request's own answer
    // cannot leave: a request landing a few milliseconds before an old
    // deadline was being restarted under, its 202 still queued.
    if (st == State::Armed && (int32_t)(_dueMs - due) < 0) due = _dueMs;
    if ((int32_t)(due - (nowMs + kAckFloorMs)) < 0) due = nowMs + kAckFloorMs;
    _dueMs = due; _target = target; _source = source;
    _state.store(State::Armed, std::memory_order_release);
    return true;
  }

  Step tick(uint32_t nowMs) {
    Guard g(_lock);
    switch (_state.load(std::memory_order_acquire)) {
      case State::Idle:
        return Step::None;
      case State::Armed:
        if ((int32_t)(nowMs - _dueMs) < 0) return Step::None;
        _state.store(State::Quiescing, std::memory_order_release);
        return Step::Quiesce;
      case State::Quiescing:
        _state.store(State::Restarting, std::memory_order_release);
        return Step::Restart;
      case State::Restarting:
        return Step::Restart;             // the caller did not manage to; ask again
    }
    return Step::None;
  }

  // Once armed, services should refuse new work: a request accepted now is a
  // request that will not be answered.
  bool pending() const { return _state.load(std::memory_order_acquire) != State::Idle; }
  State  state()  const { return _state.load(std::memory_order_acquire); }
  Pending snapshot() const {
    Guard g(_lock);
    // Field by field: with its default initialisers Pending is not an
    // aggregate under gnu++11, and a brace list here does not compile there.
    Pending p;
    p.state  = _state.load(std::memory_order_relaxed);
    p.target = _target;
    p.source = _source;
    p.dueMs  = _dueMs;
    return p;
  }

private:
  // On the chip the guard is a critical section: the holder cannot be
  // preempted, so a caller of higher priority landing on the same core —
  // the USB event task carrying a touch, unpinned and at priority 24 — waits
  // out a few instructions rather than spinning on a flag held by a task
  // that can no longer run. Off the chip (the native tests, one thread) a
  // flag is all that is needed.
  #if defined(ESP_PLATFORM)
    struct Guard {
      portMUX_TYPE& m;
      explicit Guard(portMUX_TYPE& mux) : m(mux) { taskENTER_CRITICAL(&m); }
      ~Guard() { taskEXIT_CRITICAL(&m); }
    };
    mutable portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
  #else
    struct Guard {
      std::atomic_flag& f;
      explicit Guard(std::atomic_flag& flag) : f(flag) { while (f.test_and_set(std::memory_order_acquire)) {} }
      ~Guard() { f.clear(std::memory_order_release); }
    };
    mutable std::atomic_flag _lock = ATOMIC_FLAG_INIT;
  #endif
  std::atomic<State> _state{State::Idle};
  Target   _target = Target::App;
  Source   _source = Source::Http;
  uint32_t _dueMs  = 0;
};

} // namespace Bootloader
