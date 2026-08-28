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
//  networking (CDC-NCM), PPP over a USB-UART bridge, or a wire in the
//  future. They differ in every physical detail and in nothing that matters
//  above lwIP: each one is a network interface that is either usable or not,
//  carries an address or does not, and moves some number of bytes.
//
//  This header is the part that is pure. The phase machine below is the
//  rule every link follows, whatever the hardware underneath, and it is here
//  rather than inside each driver so that it can be tested on the host and so
//  that the API reports the same vocabulary for every link type. The drivers
//  (LocalLink.h) feed it events; nothing in here touches a radio, a USB
//  peripheral or a UART.
//
//  Deliberately free of Arduino.h. Only the ESP side includes that.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

namespace LocalLink {

// What carries the link. USB_NCM and PPP_UART are the two this work exists
// for; ETHERNET is reserved so a wired board does not need a new vocabulary.
enum class Type : uint8_t { WifiAp = 0, WifiSta, UsbNcm, PppUart, RnsSerial, Ethernet };

// How the node's address on the link was decided.
enum class Addressing : uint8_t { None = 0, Static, Dhcp, LinkLocal };

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
    case Type::RnsSerial: return "rns_serial";
    case Type::Ethernet:  return "ethernet";
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
    case Addressing::LinkLocal: return "link_local";
  }
  return "unknown";
}

// Whether the link is one a host plugs into directly — the kind the
// bootloader API trusts by default. The station uplink is a LAN somebody
// else runs, so it is not, whatever its phase.
inline bool isHostFacing(Type t) {
  switch (t) {
    case Type::WifiAp: case Type::UsbNcm: case Type::PppUart:
    case Type::RnsSerial: case Type::Ethernet:
      return true;
    case Type::WifiSta:
      return false;
  }
  return false;
}

// Byte and packet counters. A driver that cannot count (the Wi-Fi stack does
// not expose per-interface totals cheaply) leaves `known` false rather than
// reporting zeros that look like silence.
struct Counters {
  uint32_t rxBytes   = 0;
  uint32_t txBytes   = 0;
  uint32_t rxPackets = 0;
  uint32_t txPackets = 0;
  uint32_t errors    = 0;
  bool     known     = false;
};

// The phase machine proper. One per link. `apply` returns true when the
// phase changed, so a driver can log transitions and nothing else.
class Machine {
public:
  explicit Machine(bool enabled = false) : _phase(enabled ? Phase::Down : Phase::Disabled) {}

  Phase phase() const { return _phase; }

  bool apply(Event e, uint32_t nowMs) {
    const Phase before = _phase;
    switch (e) {
      case Event::Enable:
        if (_phase == Phase::Disabled) _phase = Phase::Down;
        break;
      case Event::Disable:
        _phase = Phase::Disabled;
        break;
      case Event::CarrierUp:
        if (_phase == Phase::Down) _phase = Phase::Up;
        break;
      case Event::CarrierDown:
        // Losing carrier loses the address with it: an address on a link with
        // no carrier is a number nobody can reach.
        if (_phase == Phase::Up || _phase == Phase::Ready) _phase = Phase::Down;
        break;
      case Event::AddressUp:
        // A link may learn its address in the same breath as its carrier —
        // a static address is configured before the cable is in — so Down
        // goes straight to Ready rather than insisting on an Up in between.
        if (_phase == Phase::Down || _phase == Phase::Up) _phase = Phase::Ready;
        break;
      case Event::AddressDown:
        if (_phase == Phase::Ready) _phase = Phase::Up;
        break;
    }
    if (_phase == Phase::Ready && before != Phase::Ready) _readySinceMs = nowMs;
    return _phase != before;
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
  Counters   counters;
  uint32_t   uptimeS    = 0;
  uint16_t   mtu        = 0;             // 0 = not applicable / unknown
};

// Address arithmetic for policy decisions, in host order. Kept here so the
// rule "is this peer on a link we trust" is a function of numbers and can be
// tested without an IPAddress.
inline bool inSubnet(uint32_t ip, uint32_t network, uint32_t mask) {
  return (ip & mask) == (network & mask);
}

inline uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

inline void ipv4Text(uint32_t ip, char* out, size_t len) {
  snprintf(out, len, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255,
           (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

// ---------------------------------------------------------------------------
// USB networking addressing — the decision, written down once.
//
// The node takes 10.64.<n>.1/24 and the host is expected at 10.64.<n>.2, where
// <n> is derived from the node's own identity (the last octet of its factory
// MAC) rather than fixed. Two nodes on one computer then land on two subnets
// and Linux routes both without argument; with a fixed 10.64.0.0/24 the second
// one would have collided with the first. The host side is configured by the
// node — it runs a DHCP server on the link, as the Wi-Fi access point already
// does — so nothing has to be typed on the computer; the static fallback is
// there for hosts whose network manager does not ask.
//
// Why not link-local only: IPv4 link-local (169.254/16) needs the host to
// probe and the node to answer, and phones and older network managers get
// that wrong often enough that a fixed address beside it is worth having.
// ---------------------------------------------------------------------------
inline uint32_t usbSubnetFor(uint8_t macTail) { return ipv4(10, 64, macTail, 0); }
inline uint32_t usbNodeAddress(uint8_t macTail) { return ipv4(10, 64, macTail, 1); }
inline uint32_t usbHostAddress(uint8_t macTail) { return ipv4(10, 64, macTail, 2); }
constexpr uint32_t USB_SUBNET_MASK = 0xFFFFFF00u;

} // namespace LocalLink
