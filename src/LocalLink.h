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
//  LocalLink.h — the ways a host computer reaches this node
//
//  Every service the node offers a host — the web app and API on :80, the
//  Reticulum TCP server on :4242, mDNS, AutoInterface — sits on lwIP and
//  binds to every interface lwIP has. The links below are what puts an
//  interface there: the Wi-Fi access point and station today, native USB
//  (CDC-NCM) and PPP over a USB-UART bridge when the toolchain carries them.
//  None of the services know which; that is the point of the layer.
//
//                 HTTP :80 / RNS TCP :4242 / mDNS / AutoInterface
//                                      |
//                                    lwIP
//                 +--------------+-----+-------+--------------+
//              wifi_ap       wifi_sta       usb_ncm       ppp_uart
//
//  This is the ESP side: a Link interface each driver implements, a registry
//  the API and the display read snapshots from, the Wi-Fi adapters, and the
//  one policy question the bootloader API needs answered — "did this request
//  arrive over a link a host is plugged into". The phase machine and the
//  vocabulary are in LocalLinkState.h, which has no Arduino in it.
//
//  What this is not: LoRa. The radio is a Reticulum transport, and nothing
//  here bridges Ethernet or IP onto it.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"
#include "LocalLinkState.h"

namespace LocalLink {

class Link {
public:
  virtual ~Link() {}
  virtual Type        type() const = 0;
  virtual const char* name() const = 0;          // "wifi-ap", "wifi-sta", "usb0", "ppp0"
  // Whether the board has the hardware, whether this firmware build carries
  // the driver, and whether the operator has it switched on. The three are
  // reported separately so the settings page can say *why* a link is off.
  virtual bool hardware() const = 0;
  virtual bool firmware() const = 0;
  virtual bool enabled()  const = 0;
  virtual void begin() = 0;                      // bring up, if enabled and possible
  virtual void poll(uint32_t nowMs) = 0;         // refresh the phase machine from the driver
  virtual Snapshot snapshot() const = 0;
  // Host-order IPv4 network and mask the link serves; false when it has none.
  virtual bool subnet(uint32_t& network, uint32_t& mask) const = 0;
};

// The registry. Links are added once at boot in main.cpp; nothing removes
// one, so the snapshot readers need no lock beyond the copy they take.
void   add(Link* link);
size_t count();
Link*  at(size_t i);
Link*  find(Type t);
void   begin();                                  // begin() on every registered link
void   poll(uint32_t nowMs);                     // from loop()

// Copies out every link's snapshot; returns how many were written.
size_t snapshots(Snapshot* out, size_t max);

// True when `ipHostOrder` is on the subnet of a Ready, host-facing link and
// on no other link's. The bootloader API accepts requests from those links by
// default and from no other address, so a relay on somebody's LAN cannot be
// put into its downloader from across that LAN unless the operator says so —
// and a LAN that happens to be numbered like the access point is refused too,
// because an address alone cannot say which side it came in on.
bool isHostFacingAddress(uint32_t ipHostOrder);

// An IPAddress as a host-order number, for inSubnet() and friends.
uint32_t hostOrder(const IPAddress& a);

// The Wi-Fi adapters. Constructed in main.cpp so the boot order is visible
// in one place; WifiManager still owns the radio, and these only observe it.
class WifiApLink : public Link {
public:
  Type type() const override { return Type::WifiAp; }
  const char* name() const override { return "wifi-ap"; }
  bool hardware() const override { return true; }
  bool firmware() const override { return true; }
  bool enabled()  const override;
  void begin() override;
  void poll(uint32_t nowMs) override;
  Snapshot snapshot() const override;
  bool subnet(uint32_t& network, uint32_t& mask) const override;
private:
  Machine  _m;
  uint32_t _nowMs = 0;
};

class WifiStaLink : public Link {
public:
  Type type() const override { return Type::WifiSta; }
  const char* name() const override { return "wifi-sta"; }
  bool hardware() const override { return true; }
  bool firmware() const override { return true; }
  bool enabled()  const override;
  void begin() override;
  void poll(uint32_t nowMs) override;
  Snapshot snapshot() const override;
  bool subnet(uint32_t& network, uint32_t& mask) const override;
private:
  Machine  _m;
  uint32_t _nowMs = 0;
};

// Links this board could carry but this build does not: they appear in the
// registry so the API lists them with hardware=true, firmware=false and a
// reason, and the settings page can show the switch greyed out rather than
// absent. Nothing else about them runs.
class UnavailableLink : public Link {
public:
  UnavailableLink(Type t, const char* name, bool hardware, const char* reason)
    : _type(t), _name(name), _hardware(hardware), _reason(reason) {}
  Type type() const override { return _type; }
  const char* name() const override { return _name; }
  bool hardware() const override { return _hardware; }
  bool firmware() const override { return false; }
  bool enabled()  const override { return false; }
  void begin() override {}
  void poll(uint32_t) override {}
  Snapshot snapshot() const override;
  bool subnet(uint32_t&, uint32_t&) const override { return false; }
  const char* reason() const { return _reason; }
private:
  Type        _type;
  const char* _name;
  bool        _hardware;
  const char* _reason;
};

// Why a link the board has cannot be used in this build, for the API. Empty
// for links that work.
const char* unavailableReason(const Link& link);

} // namespace LocalLink
