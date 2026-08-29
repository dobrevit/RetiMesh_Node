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
#include "Bootloader.h"

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

bool requestIsHostFacing(uint32_t localIp, uint32_t remoteIp) {
  const Link* l = serving(localIp);
  if (!l || !isHostFacing(l->type())) return false;
  const uint32_t mask = l->netmask();
  return mask != 0 && (remoteIp & mask) == (l->address() & mask);
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

Apply applyLinks(const LinkSettings& want, const bool* changed, Bootloader_Source source, const char** detail) {
  if (detail) *detail = "";
  // The same refusal every HTTP write gives while a restart is armed: a
  // setting saved now might not reach flash before it.
  if (Bootloader::pending()) return Apply::RefusedBusy;
  LinkSettings next = settings.links();
  size_t n = 0;
  const Field* f = fields(n);
  for (size_t i = 0; i < n; i++) {
    if (!changed[i]) continue;
    const bool on = want.*(f[i].on);
    const Link* link = find(f[i].type);
    if (on && (!link || !link->usable())) {
      if (detail) *detail = link ? link->reason() : "no such link on this board";
      return Apply::RefusedUnusable;
    }
    next.*(f[i].on) = on;
  }
  if (lockedOut(next, settings.maintenance().consoleEnabled)) return Apply::RefusedLockedOut;
  const bool wifiChanged = next.wifiEnabled != settings.links().wifiEnabled;
  bool same = true;
  for (size_t i = 0; i < n; i++) if (next.*(f[i].on) != settings.links().*(f[i].on)) same = false;
  if (same) return Apply::Unchanged;
  if (!settings.saveLinks(next)) return Apply::NvsFailed;
  if (!wifiChanged) return Apply::Saved;
  // The access point cannot be torn down under the request that asked, so a
  // Wi-Fi change takes effect at a restart; whether one is granted now is the
  // bootloader manager's answer.
  return Bootloader::reboot(source) ? Apply::SavedRestarting : Apply::SavedNextBoot;
}

// ---------------------------------------------------------------------------
// Wi-Fi adapters
// ---------------------------------------------------------------------------
void WifiLink::begin() {
  _applied = wanted();
  _m = Machine(_applied);
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

uint32_t WifiLink::netmask() const {
  return _m.phase() == Phase::Ready ? hostOrder(mask()) : 0;
}

bool      WifiApLink::wanted() const  { return settings.links().wifiEnabled; }
bool      WifiApLink::carrier() const { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }
IPAddress WifiApLink::ip() const      { return WiFi.softAPIP(); }
IPAddress WifiApLink::mask() const    { return WiFi.softAPSubnetMask(); }
uint8_t   WifiApLink::clients() const { return WiFi.softAPgetStationNum(); }

bool      WifiStaLink::wanted() const  { return settings.links().wifiEnabled && wifiManager.stationConfigured(); }
bool      WifiStaLink::carrier() const { return wifiManager.stationConnected(); }
IPAddress WifiStaLink::ip() const      { return WiFi.localIP(); }
IPAddress WifiStaLink::mask() const    { return WiFi.subnetMask(); }

// ---------------------------------------------------------------------------
Snapshot UnavailableLink::snapshot() const {
  Snapshot s;
  s.type = _type;
  strlcpy(s.name, _name, sizeof(s.name));
  s.phase = Phase::Disabled;
  return s;
}

} // namespace LocalLink
