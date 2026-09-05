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
#include "UsbNcm.h"
#include <lwip/def.h>
#include "PppUart.h"
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

// The default: an address is on this link when it falls inside the link's
// subnet. A point-to-point link has no subnet to fall inside and overrides
// this — see PppLink, whose one peer is the only address it answers for.
bool Link::remoteOnLink(uint32_t remoteIp) const {
  const uint32_t mask = netmask();
  return mask != 0 && (remoteIp & mask) == (address() & mask);
}

bool requestIsOnItsLink(uint32_t localIp, uint32_t remoteIp) {
  const Link* l = serving(localIp);
  return l && l->remoteOnLink(remoteIp);
}

bool requestIsHostFacing(uint32_t localIp, uint32_t remoteIp) {
  const Link* l = serving(localIp);
  return l && isHostFacing(l->type()) && requestIsOnItsLink(localIp, remoteIp);
}

uint32_t hostOrder(const IPAddress& a) { return ipv4(a[0], a[1], a[2], a[3]); }

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
const Field* fields(size_t& n) {
  static const Field kFields[] = {
    // Two Wi-Fi rows now, one per link, because the two are switched apart.
    // The old single "wifi" key is still understood by the settings table,
    // where it writes both — but here each link maps to its own switch, or the
    // API would report the station's state for the access point.
    { "wifi_ap",  Type::WifiAp,  &LinkSettings::wifiApEnabled  },
    { "wifi_sta", Type::WifiSta, &LinkSettings::wifiStaEnabled },
    { "usb",      Type::UsbNcm,  &LinkSettings::usbEnabled     },
    { "ppp",      Type::PppUart, &LinkSettings::pppEnabled     },
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

bool lockedOut(const LinkSettings& l, bool consoleEnabled, bool webUi) {
  return wouldLockOut(anySwitchOn(l), consoleEnabled, webUi);   // the rule is in LocalLinkState.h
}

const uint32_t* pppBauds(size_t& n) {
  // The registry's ladder for this board, as tools/board_caps.py hands it
  // to the build: a brace-initialiser body.
  static const uint32_t kBauds[] = { BOARD_UART_BAUDS };
  n = sizeof(kBauds) / sizeof(kBauds[0]);
  return kBauds;
}

bool pppBaudUsable(uint32_t baud) {
  size_t n = 0;
  const uint32_t* ladder = pppBauds(n);
  return pppBaudAllowed(baud, ladder, n, BOARD_UART_MAX_BAUD);
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
  // The PPP speed travels with the switches: one save, one answer. It is
  // judged by the board's rule, not by whether PPP is on — a speed saved
  // while PPP is off is the one PPP will run at when it is switched on.
  if (want.pppBaud != next.pppBaud) {
    const Link* ppp = find(Type::PppUart);
    if (!ppp || !ppp->usable()) {
      if (detail) *detail = ppp ? ppp->reason() : "no PPP link on this board";
      return Apply::RefusedUnusable;
    }
    if (!pppBaudUsable(want.pppBaud)) {
      if (detail) *detail = "not a speed this board is qualified for; the settings list the ones it is";
      return Apply::RefusedBaud;
    }
    next.pppBaud = want.pppBaud;
  }
  if (lockedOut(next, settings.maintenance().consoleEnabled, settings.maintenance().webUi))
    return Apply::RefusedLockedOut;
  // Either link changing needs the restart, not only the pair as a whole: the
  // radio's mode is chosen once at bring-up, so turning the access point off
  // while the station stays on is still a change the running radio cannot make.
  const bool wifiChanged = next.wifiApEnabled  != settings.links().wifiApEnabled ||
                           next.wifiStaEnabled != settings.links().wifiStaEnabled;
  bool same = next.pppBaud == settings.links().pppBaud;
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
// The phase machine, for every link that runs
// ---------------------------------------------------------------------------
void MachineLink::begin() {
  _m = Machine(enabled());
  poll(millis());
}

void MachineLink::poll(uint32_t nowMs) {
  drive();
  if (!enabled()) { _m.apply(Event::Disable, nowMs); return; }
  _m.apply(Event::Enable, nowMs);
  const bool up = carrier();
  _m.apply(up ? Event::CarrierUp : Event::CarrierDown, nowMs);
  if (up) _m.apply((uint32_t)ip() != 0 ? Event::AddressUp : Event::AddressDown, nowMs);
}

Snapshot MachineLink::snapshot() const {
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

uint32_t MachineLink::address() const {
  return _m.phase() == Phase::Ready ? hostOrder(ip()) : 0;
}

uint32_t MachineLink::netmask() const {
  return _m.phase() == Phase::Ready ? hostOrder(mask()) : 0;
}

// ---------------------------------------------------------------------------
// Wi-Fi adapters
// ---------------------------------------------------------------------------
bool      WifiApLink::wanted() const  { return settings.links().wifiApEnabled; }
bool      WifiApLink::carrier() const { return (WiFi.getMode() & WIFI_MODE_AP) != 0; }
IPAddress WifiApLink::ip() const      { return WiFi.softAPIP(); }
IPAddress WifiApLink::mask() const    { return WiFi.softAPSubnetMask(); }
uint8_t   WifiApLink::clients() const { return WiFi.softAPgetStationNum(); }

bool      WifiStaLink::wanted() const  { return settings.links().wifiStaEnabled && wifiManager.stationConfigured(); }
bool      WifiStaLink::carrier() const { return wifiManager.stationConnected(); }
IPAddress WifiStaLink::ip() const      { return WiFi.localIP(); }
IPAddress WifiStaLink::mask() const    { return WiFi.subnetMask(); }

// ---------------------------------------------------------------------------
// USB
// ---------------------------------------------------------------------------
#if HAS_USB_NCM
bool UsbNcmLink::enabled() const { return settings.links().usbEnabled; }
void UsbNcmLink::begin() { UsbNcm::begin(); MachineLink::begin(); }
// The driver follows the switch every pass, so a change saved by the API or
// the console takes effect here, on the loop task, where the network stack
// may be called.
void      UsbNcmLink::drive()         { UsbNcm::poll(enabled()); }
bool      UsbNcmLink::carrier() const { return UsbNcm::linkUp(); }
IPAddress UsbNcmLink::ip() const      { return UsbNcm::address(); }
IPAddress UsbNcmLink::mask() const    { return IPAddress(htonl(kUsbNetmask)); }
#endif

// ---------------------------------------------------------------------------
// PPP
// ---------------------------------------------------------------------------
#if HAS_PPP
bool PppLink::enabled() const { return settings.links().pppEnabled; }
void PppLink::begin() { PppUart::begin(); MachineLink::begin(); }

// The driver follows the switch and the speed every pass, on the loop task,
// so a change saved by the API or the console takes effect here.
void PppLink::drive() { PppUart::poll(enabled(), settings.links().pppBaud); }

// Carrier is the host having opened PPP — the port is PPP's — and the address
// follows when IPCP finishes, which is the machine's own next step. Down is a
// port the console owns: nothing wrong, nobody dialling.
bool      PppLink::carrier() const { return PppUart::owner() == PppUart::Owner::Ppp; }
IPAddress PppLink::ip() const      { return PppUart::address(); }
// What lwIP puts on a point-to-point interface: the one address.
IPAddress PppLink::mask() const    { return IPAddress(0xFFFFFFFFu); }

bool PppLink::remoteOnLink(uint32_t remoteIp) const {
  // The subnet rule would refuse the peer — a /32 contains only ourselves —
  // and the peer is the only address the wire can carry.
  // address() is non-zero only once the machine is Ready, which is the
  // condition this needs and the base class already answers.
  return address() != 0 && remoteIp != 0 && remoteIp == hostOrder(PppUart::peer());
}
#endif

// ---------------------------------------------------------------------------
Snapshot UnavailableLink::snapshot() const {
  Snapshot s;
  s.type = _type;
  strlcpy(s.name, _name, sizeof(s.name));
  s.phase = Phase::Disabled;
  return s;
}

} // namespace LocalLink
