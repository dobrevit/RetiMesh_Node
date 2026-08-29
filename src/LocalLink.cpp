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
//  LocalLink.cpp — the registry, the settings table and the Wi-Fi adapters.
//  See LocalLink.h.
// ============================================================================
#include "LocalLink.h"
#include <WiFi.h>
#include "WifiManager.h"

namespace LocalLink {

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
static Link*  sLinks[kMaxLinks];
static size_t sCount = 0;

void add(Link* link) {
  if (link && sCount < kMaxLinks) sLinks[sCount++] = link;
}
size_t count() { return sCount; }
Link*  at(size_t i) { return i < sCount ? sLinks[i] : nullptr; }
Link*  find(Type t) {
  for (size_t i = 0; i < sCount; i++) if (sLinks[i]->type() == t) return sLinks[i];
  return nullptr;
}
void begin() { for (size_t i = 0; i < sCount; i++) sLinks[i]->begin(); }
void poll(uint32_t nowMs) { for (size_t i = 0; i < sCount; i++) sLinks[i]->poll(nowMs); }

Link* serving(uint32_t localIp) {
  if (!localIp) return nullptr;
  for (size_t i = 0; i < sCount; i++)
    if (sLinks[i]->address() == localIp) return sLinks[i];
  return nullptr;
}

bool requestIsHostFacing(uint32_t localIp) {
  const Link* l = serving(localIp);
  return l && isHostFacing(l->type());
}

uint32_t hostOrder(const IPAddress& a) { return ipv4(a[0], a[1], a[2], a[3]); }

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
const Field* fields(size_t& n) {
  static const Field kFields[] = {
    { "wifi", Type::WifiAp,  &LinkSettings::wifiEnabled },
    { "usb",  Type::UsbNcm,  &LinkSettings::usbEnabled  },
    { "ppp",  Type::PppUart, &LinkSettings::pppEnabled  },
  };
  n = sizeof(kFields) / sizeof(kFields[0]);
  return kFields;
}

bool switchOn(const Link& l, const LinkSettings& s) {
  if (!l.usable()) return false;
  size_t n = 0;
  const Field* f = fields(n);
  for (size_t i = 0; i < n; i++) if (f[i].type == l.type()) return s.*(f[i].on);
  return false;
}

bool anySwitchOn(const LinkSettings& l) {
  for (size_t i = 0; i < sCount; i++)
    if (switchOn(*sLinks[i], l)) return true;
  return false;
}

bool lockedOut(const LinkSettings& l, bool consoleEnabled) {
  return !consoleEnabled && !anySwitchOn(l);
}

// ---------------------------------------------------------------------------
// Wi-Fi adapters
// ---------------------------------------------------------------------------
void WifiLink::begin() {
  _m = Machine(enabled());
  poll(millis());
}

void WifiLink::poll(uint32_t nowMs) {
  if (!enabled()) { _m.apply(Event::Disable, nowMs); return; }
  _m.apply(Event::Enable, nowMs);
  const bool up = carrier();
  _m.apply(up ? Event::CarrierUp : Event::CarrierDown, nowMs);
  if (up) _m.apply((uint32_t)ip() != 0 ? Event::AddressUp : Event::AddressDown, nowMs);
}

Snapshot WifiLink::snapshot() const {
  Snapshot s;
  s.type = type();
  strlcpy(s.name, name(), sizeof(s.name));
  s.phase = _m.phase();
  s.uptimeS = _m.uptimeS(millis());
  if (s.phase == Phase::Ready) {
    s.addressing = addressing();
    strlcpy(s.ip, ip().toString().c_str(), sizeof(s.ip));
    s.clientKnown = clientsKnown();
    s.clients = clients();
  }
  return s;
}

uint32_t WifiLink::address() const {
  return _m.phase() == Phase::Ready ? hostOrder(ip()) : 0;
}

bool      WifiApLink::enabled() const { return settings.links().wifiEnabled; }
bool      WifiApLink::carrier() const { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }
IPAddress WifiApLink::ip() const      { return WiFi.softAPIP(); }
uint8_t   WifiApLink::clients() const { return WiFi.softAPgetStationNum(); }

bool      WifiStaLink::enabled() const { return settings.links().wifiEnabled && wifiManager.stationConfigured(); }
bool      WifiStaLink::carrier() const { return wifiManager.stationConnected(); }
IPAddress WifiStaLink::ip() const      { return WiFi.localIP(); }

// ---------------------------------------------------------------------------
Snapshot UnavailableLink::snapshot() const {
  Snapshot s;
  s.type = _type;
  strlcpy(s.name, _name, sizeof(s.name));
  s.phase = Phase::Disabled;
  return s;
}

} // namespace LocalLink
