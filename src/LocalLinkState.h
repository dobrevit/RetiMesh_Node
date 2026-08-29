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
//  LocalLinkState.h — what a local link is, and the state it can be in
//
//  A local link is one of the ways a host computer reaches this node
//  directly: the Wi-Fi access point, the station uplink, native USB
//  networking (CDC-NCM) or PPP over a USB-UART bridge. They differ in every
//  physical detail and in nothing that matters above lwIP: each one is a
//  network interface that is either usable or not, and carries an address or
//  does not.
//
//  This header is the part that is pure. The phase machine below is the
//  rule every link follows, whatever the hardware underneath, and it is here
//  rather than inside each driver so that it can be tested on the host and so
//  that the API reports the same vocabulary for every link type. The drivers
//  (LocalLink.h) feed it events; nothing in here touches a radio, a USB
//  peripheral or a UART.
//
//  What is deliberately not here: byte counters, an MTU, and the addressing
//  plan for USB networking. An earlier version carried all three as fields
//  that no driver could fill in, and every reader then grew an "unknown"
//  branch around them. They arrive with the driver that produces the numbers.
//
//  Deliberately free of Arduino.h. Only the ESP side includes that.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace LocalLink {

// What carries the link. The two beyond Wi-Fi are the ones this work exists
// for; nothing is reserved for hardware that does not exist yet, because a
// name with no implementation behind it is one more case every switch has
// to mention and no test can exercise.
enum class Type : uint8_t { WifiAp = 0, WifiSta, UsbNcm, PppUart };

// How the node's address on the link was decided.
enum class Addressing : uint8_t { None = 0, Static, Dhcp };

// The phase machine. Ready is the only phase in which a service can be
// reached over the link; everything else is a reason it cannot.
//
//   Disabled  the operator turned the link off (or the board cannot do it)
//   Down      enabled, no carrier: cable out, radio off, host asleep
//   Up        carrier present, no address yet: link up, DHCP pending
//   Ready     up with an address: reachable
enum class Phase : uint8_t { Disabled = 0, Down, Up, Ready };

enum class Event : uint8_t {
  Enable, Disable,
  CarrierUp, CarrierDown,      // the physical/logical link came or went
  AddressUp, AddressDown,      // an address was assigned or withdrawn
};

inline const char* typeName(Type t) {
  switch (t) {
    case Type::WifiAp:    return "wifi_ap";
    case Type::WifiSta:   return "wifi_sta";
    case Type::UsbNcm:    return "usb_ncm";
    case Type::PppUart:   return "ppp_uart";
  }
  return "unknown";
}

inline const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Disabled: return "disabled";
    case Phase::Down:     return "down";
    case Phase::Up:       return "up";
    case Phase::Ready:    return "ready";
  }
  return "unknown";
}

inline const char* addressingName(Addressing a) {
  switch (a) {
    case Addressing::None:      return "none";
    case Addressing::Static:    return "static";
    case Addressing::Dhcp:      return "dhcp";
  }
  return "unknown";
}

// Whether the link is one a host plugs into directly — the kind the
// bootloader API trusts by default. The station uplink is a LAN somebody
// else runs, so it is not, whatever its phase.
inline bool isHostFacing(Type t) {
  switch (t) {
    case Type::WifiAp: case Type::UsbNcm: case Type::PppUart:
      return true;
    case Type::WifiSta:
      return false;
  }
  return false;
}

// The phase machine proper. One per link. `apply` returns true when the
// phase changed, so a driver can log transitions and nothing else.
class Machine {
public:
  explicit Machine(bool enabled = false) : _phase(enabled ? Phase::Down : Phase::Disabled) {}

  Phase phase() const { return _phase; }

  bool apply(Event e, uint32_t nowMs) {
    const Phase before = _phase;
    Phase next = before;
    switch (e) {
      case Event::Enable:
        if (before == Phase::Disabled) next = Phase::Down;
        break;
      case Event::Disable:
        next = Phase::Disabled;
        break;
      case Event::CarrierUp:
        if (before == Phase::Down) next = Phase::Up;
        break;
      case Event::CarrierDown:
        // Losing carrier loses the address with it: an address on a link with
        // no carrier is a number nobody can reach.
        if (before == Phase::Up || before == Phase::Ready) next = Phase::Down;
        break;
      case Event::AddressUp:
        // A link may learn its address in the same breath as its carrier —
        // a static address is configured before the cable is in — so Down
        // goes straight to Ready rather than insisting on an Up in between.
        if (before == Phase::Down || before == Phase::Up) next = Phase::Ready;
        break;
      case Event::AddressDown:
        if (before == Phase::Ready) next = Phase::Up;
        break;
    }
    // The timestamp is settled before the phase is published, because a
    // reader on another task copies phase first and then asks for uptime: the
    // other order let it see Ready with the previous epoch's start.
    if (next == Phase::Ready && before != Phase::Ready) _readySinceMs = nowMs;
    _phase = next;
    return next != before;
  }

  // Seconds the link has been reachable for; 0 unless Ready.
  uint32_t uptimeS(uint32_t nowMs) const {
    return _phase == Phase::Ready ? (nowMs - _readySinceMs) / 1000 : 0;
  }

private:
  Phase    _phase;
  uint32_t _readySinceMs = 0;
};

// Everything the API and the display want to know about one link, copied out
// so a reader never holds a driver's internals.
struct Snapshot {
  Type       type       = Type::WifiAp;
  char       name[16]   = "";            // "wifi-ap", "usb0", "ppp0" ...
  Phase      phase      = Phase::Disabled;
  Addressing addressing = Addressing::None;
  char       ip[40]     = "";            // text; "" when none
  bool       clientKnown = false;        // whether `clients` means anything
  uint8_t    clients    = 0;             // hosts attached, where the link can tell
  uint32_t   uptimeS    = 0;
};

// An IPv4 address as one host-order number, so two of them can be compared.
inline uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

} // namespace LocalLink
