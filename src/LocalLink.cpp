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
//  LocalLink.cpp — the registry and the Wi-Fi adapters. See LocalLink.h.
// ============================================================================
#include "LocalLink.h"
#include <WiFi.h>
#include "Settings.h"
#include "WifiManager.h"

namespace LocalLink {

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
static Link*  sLinks[6];
static size_t sCount = 0;

void add(Link* link) {
  if (link && sCount < sizeof(sLinks) / sizeof(sLinks[0])) sLinks[sCount++] = link;
}
size_t count() { return sCount; }
Link*  at(size_t i) { return i < sCount ? sLinks[i] : nullptr; }
Link*  find(Type t) {
  for (size_t i = 0; i < sCount; i++) if (sLinks[i]->type() == t) return sLinks[i];
  return nullptr;
}
void begin() { for (size_t i = 0; i < sCount; i++) sLinks[i]->begin(); }
void poll(uint32_t nowMs) { for (size_t i = 0; i < sCount; i++) sLinks[i]->poll(nowMs); }

size_t snapshots(Snapshot* out, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < sCount && n < max; i++) out[n++] = sLinks[i]->snapshot();
  return n;
}

bool isHostFacingAddress(uint32_t ip) {
  for (size_t i = 0; i < sCount; i++) {
    const Link* l = sLinks[i];
    if (!isHostFacing(l->type())) continue;
    if (l->snapshot().phase != Phase::Ready) continue;
    uint32_t net, mask;
    if (l->subnet(net, mask) && inSubnet(ip, net, mask)) return true;
  }
  return false;
}

const char* unavailableReason(const Link& link) {
  if (link.firmware()) return "";
  const UnavailableLink* u = static_cast<const UnavailableLink*>(&link);
  return u->reason();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t hostOrder(const IPAddress& a) { return ipv4(a[0], a[1], a[2], a[3]); }

static void fillCommon(Snapshot& s, const Link& l, const Machine& m, uint32_t nowMs) {
  s.type = l.type();
  strlcpy(s.name, l.name(), sizeof(s.name));
  s.phase = m.phase();
  s.uptimeS = m.uptimeS(nowMs);
  // The Wi-Fi stack keeps no per-interface byte totals the application can
  // read without LWIP_STATS, which the prebuilt core leaves off. Saying so
  // beats printing zeros next to a link that is plainly carrying traffic.
  s.counters.known = false;
  s.mtu = 1500;
}

// ---------------------------------------------------------------------------
// Access point
// ---------------------------------------------------------------------------
bool WifiApLink::enabled() const { return settings.links().wifiEnabled; }

void WifiApLink::begin() {
  _m = Machine(enabled());
  poll(millis());
}

void WifiApLink::poll(uint32_t nowMs) {
  _nowMs = nowMs;
  if (!enabled()) { _m.apply(Event::Disable, nowMs); return; }
  _m.apply(Event::Enable, nowMs);
  const bool apUp = (WiFi.getMode() & WIFI_MODE_AP) != 0;
  _m.apply(apUp ? Event::CarrierUp : Event::CarrierDown, nowMs);
  if (apUp) {
    const bool addressed = (uint32_t)WiFi.softAPIP() != 0;
    _m.apply(addressed ? Event::AddressUp : Event::AddressDown, nowMs);
  }
}

Snapshot WifiApLink::snapshot() const {
  Snapshot s;
  fillCommon(s, *this, _m, _nowMs);
  if (s.phase == Phase::Ready) {
    s.addressing = Addressing::Static;
    strlcpy(s.ip, WiFi.softAPIP().toString().c_str(), sizeof(s.ip));
    s.clientKnown = true;
    s.clients = WiFi.softAPgetStationNum();
  }
  return s;
}

bool WifiApLink::subnet(uint32_t& network, uint32_t& mask) const {
  if (_m.phase() != Phase::Ready) return false;
  network = hostOrder(WiFi.softAPIP());
  mask    = hostOrder(WiFi.softAPSubnetMask());
  return true;
}

// ---------------------------------------------------------------------------
// Station
// ---------------------------------------------------------------------------
bool WifiStaLink::enabled() const {
  return settings.links().wifiEnabled && settings.wifi().staSsid[0] != '\0';
}

void WifiStaLink::begin() {
  _m = Machine(enabled());
  poll(millis());
}

void WifiStaLink::poll(uint32_t nowMs) {
  _nowMs = nowMs;
  if (!enabled()) { _m.apply(Event::Disable, nowMs); return; }
  _m.apply(Event::Enable, nowMs);
  const bool connected = WiFi.status() == WL_CONNECTED;
  _m.apply(connected ? Event::CarrierUp : Event::CarrierDown, nowMs);
  if (connected) {
    const bool addressed = (uint32_t)WiFi.localIP() != 0;
    _m.apply(addressed ? Event::AddressUp : Event::AddressDown, nowMs);
  }
}

Snapshot WifiStaLink::snapshot() const {
  Snapshot s;
  fillCommon(s, *this, _m, _nowMs);
  if (s.phase == Phase::Ready) {
    s.addressing = Addressing::Dhcp;
    strlcpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip));
  }
  return s;
}

bool WifiStaLink::subnet(uint32_t& network, uint32_t& mask) const {
  if (_m.phase() != Phase::Ready) return false;
  network = hostOrder(WiFi.localIP());
  mask    = hostOrder(WiFi.subnetMask());
  return true;
}

// ---------------------------------------------------------------------------
Snapshot UnavailableLink::snapshot() const {
  Snapshot s;
  s.type = _type;
  strlcpy(s.name, _name, sizeof(s.name));
  s.phase = Phase::Disabled;
  return s;
}

} // namespace LocalLink
