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
//  Three pieces, all pure:
//    plan()         which methods a board offers, best first
//    Sequencer      the request -> quiesce -> restart steps, with the delay
//                   that lets an acknowledgement leave before the link dies
//    TouchDetector  the 1200-baud CDC trigger, with the debounce that keeps a
//                   console opened at any other rate from rebooting the node
//
//  The mechanism the software path relies on is documented rather than
//  guessed: ESP32-S2/S3/C3 carry RTC_CNTL_FORCE_DOWNLOAD_BOOT in
//  RTC_CNTL_OPTION1_REG, and writing it in a shutdown handler before
//  esp_restart() is exactly what the Arduino core's usb_persist_restart()
//  does for its own 1200-baud touch. The classic ESP32 has no such bit, so a
//  running node there cannot put itself into the downloader; only the bridge
//  or a finger can. Bootloader.cpp is where the register is actually written.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Bootloader {

enum class Method : uint8_t {
  Usb1200Touch,     // host opens the CDC-ACM port at the magic baud and closes it
  SoftwareApi,      // firmware sets FORCE_DOWNLOAD_BOOT and restarts (HTTP or console)
  AutoResetDtrRts,  // esptool drives EN/IO0 through the bridge or USB-Serial/JTAG
  ManualRecovery,   // hold BOOT, press RESET
};

inline const char* methodName(Method m) {
  switch (m) {
    case Method::Usb1200Touch:    return "usb_1200_touch";
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
  bool usbCdcOtg         = false;   // firmware runs a TinyUSB CDC-ACM that sees line coding
  bool bridgeAutoReset   = false;   // a USB-UART bridge with DTR/RTS wired to EN/IO0
};

struct Plan {
  Method methods[4];
  size_t count = 0;
  bool has(Method m) const {
    for (size_t i = 0; i < count; i++) if (methods[i] == m) return true;
    return false;
  }
  Method primary() const { return methods[0]; }
};

// Best first. The touch is preferred because it needs no credentials and no
// address; the API second because it works over any local link; the bridge
// reset third because esptool does it on its own; and recovery is always
// there, last, because it always works.
inline Plan plan(const Caps& c) {
  Plan p;
  if (c.usbCdcOtg && c.nativeUsb)      p.methods[p.count++] = Method::Usb1200Touch;
  if (c.forceDownloadBoot)             p.methods[p.count++] = Method::SoftwareApi;
  // The S3's USB-Serial/JTAG implements the same DTR/RTS handshake in
  // hardware, so native USB without an OTG stack counts as a bridge here.
  if (c.bridgeAutoReset || (c.nativeUsb && !c.usbCdcOtg))
                                       p.methods[p.count++] = Method::AutoResetDtrRts;
  p.methods[p.count++] = Method::ManualRecovery;
  return p;
}

// Whether firmware can get the chip into the downloader on its own, which is
// what the HTTP endpoint and the console command promise.
inline bool canEnterAutomatically(const Caps& c) { return c.forceDownloadBoot; }

// ---------------------------------------------------------------------------
// The request sequence
// ---------------------------------------------------------------------------
enum class Target : uint8_t { App = 0, Bootloader = 1 };
enum class Source : uint8_t { Http = 0, Console, UsbTouch, Settings, Button };

inline const char* targetName(Target t) { return t == Target::Bootloader ? "bootloader" : "app"; }
inline const char* sourceName(Source s) {
  switch (s) {
    case Source::Http:     return "http";
    case Source::Console:  return "console";
    case Source::UsbTouch: return "usb_touch";
    case Source::Settings: return "settings";
    case Source::Button:   return "button";
  }
  return "unknown";
}

enum class State : uint8_t { Idle = 0, Armed, Quiescing, Restarting };

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
class Sequencer {
public:
  bool request(Target target, Source source, uint32_t delayMs, uint32_t nowMs) {
    if (_state == State::Quiescing || _state == State::Restarting) return false;
    if (_state == State::Armed && _target == Target::Bootloader && target == Target::App) return false;
    _target = target;
    _source = source;
    _state  = State::Armed;
    _dueMs  = nowMs + delayMs;
    return true;
  }

  Step tick(uint32_t nowMs) {
    switch (_state) {
      case State::Idle:
        return Step::None;
      case State::Armed:
        if ((int32_t)(nowMs - _dueMs) < 0) return Step::None;
        _state = State::Quiescing;
        return Step::Quiesce;
      case State::Quiescing:
        _state = State::Restarting;
        return Step::Restart;
      case State::Restarting:
        return Step::Restart;             // the caller did not manage to; ask again
    }
    return Step::None;
  }

  // Once armed, services should refuse new work: a request accepted now is a
  // request that will not be answered.
  bool pending() const { return _state != State::Idle; }
  State  state()  const { return _state; }
  Target target() const { return _target; }
  Source source() const { return _source; }
  uint32_t dueMs() const { return _dueMs; }

private:
  State    _state  = State::Idle;
  Target   _target = Target::App;
  Source   _source = Source::Http;
  uint32_t _dueMs  = 0;
};

// ---------------------------------------------------------------------------
// The 1200-baud touch
//
// What the host does, in USB CDC terms: SET_LINE_CODING with a bit rate of
// 1200, then SET_CONTROL_LINE_STATE with DTR asserted (the port is open), and
// then DTR deasserted (the port is closed). That is what `stty -F
// /dev/ttyACM0 1200 hupcl` followed by nothing, or a Python
// `serial.Serial(port, 1200)` immediately closed, produces — and what the
// Arduino IDE has sent to reset boards since the Leonardo.
//
// What fires: DTR going from asserted to deasserted while the port's line
// coding is at the magic rate, within `windowMs` of that rate being set. A
// port opened at 115200 for a console can be opened and closed all day. A
// line coding at 1200 that is never followed by a close does nothing. A rate
// set and then changed before the close disarms. No payload byte is ever
// looked at, so no data pattern can trigger it.
// ---------------------------------------------------------------------------
class TouchDetector {
public:
  explicit TouchDetector(uint32_t magicBaud = 1200, uint32_t windowMs = 5000)
    : _magic(magicBaud), _windowMs(windowMs) {}

  void onLineCoding(uint32_t baud, uint32_t nowMs) {
    if (baud == _magic) { _armed = true; _armedAtMs = nowMs; _dtrSeen = false; }
    else                { _armed = false; }
  }

  // Returns true exactly once per touch: the bootloader should be entered.
  bool onLineState(bool dtr, bool rts, uint32_t nowMs) {
    (void)rts;                            // the touch is defined on DTR alone
    if (_armed && (uint32_t)(nowMs - _armedAtMs) > _windowMs) _armed = false;
    if (!_armed) { _dtrSeen = false; return false; }
    if (dtr) { _dtrSeen = true; return false; }
    // DTR fell while armed. Only a close that follows an open counts: a host
    // that sets 1200 with DTR already low never opened the port at all.
    if (!_dtrSeen) return false;
    _armed = false;
    _dtrSeen = false;
    return true;
  }

  bool armed() const { return _armed; }
  uint32_t magicBaud() const { return _magic; }

private:
  uint32_t _magic;
  uint32_t _windowMs;
  bool     _armed = false;
  bool     _dtrSeen = false;
  uint32_t _armedAtMs = 0;
};

} // namespace Bootloader
