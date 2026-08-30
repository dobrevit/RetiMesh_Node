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
//  the API and the display read snapshots from, the Wi-Fi adapters, the one
//  table that binds a link to its settings key, and the one policy question
//  the bootloader API needs answered — "did this request arrive over a link
//  a host is plugged into". The phase machine and the vocabulary are in
//  LocalLinkState.h, which has no Arduino in it.
//
//  What this is not: LoRa. The radio is a Reticulum transport, and nothing
//  here bridges Ethernet or IP onto it.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"
#include "Settings.h"
#include "LocalLinkState.h"
#include "BootloaderPlan.h"

// The restart source a link change is attributed to; spelled here so the
// header stays free of Bootloader.h.
using Bootloader_Source = Bootloader::Source;

namespace LocalLink {

// The registry holds at most this many. One constant, because the console
// once sized its own copy of the snapshot array from a second literal.
constexpr size_t kMaxLinks = 6;

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
  // The node's own IPv4 address on this link and the link's netmask, host
  // order; both 0 unless Ready.
  virtual uint32_t address() const = 0;
  virtual uint32_t netmask() const = 0;
  // Why a link the board has cannot run in this build. Empty for links that
  // can. A virtual, because the alternative — casting on the strength of
  // firmware() being false — was sound only while one class answered false.
  virtual const char* reason() const { return ""; }

  // The board has it and this build can drive it: the one predicate every
  // settings handler asks before it will save a switch as "on".
  bool usable() const { return hardware() && firmware(); }
};

// The registry. Links are added once at boot in main.cpp; nothing removes
// one, so the snapshot readers need no lock beyond the copy they take.
void   add(Link* link);
size_t count();
Link*  at(size_t i);
Link*  find(Type t);
void   begin();                                  // begin() on every registered link
void   poll(uint32_t nowMs);                     // from loop()

// The link a connection was accepted on, found by the local address it was
// accepted at — which the TCP stack knows exactly, unlike the remote address,
// which only hints. nullptr when no Ready link holds that address.
Link* serving(uint32_t localIpHostOrder);

// True when a request accepted at `localIp`, from `remoteIp`, came in over
// a host-facing link. The bootloader API accepts requests from those and
// from no other, so a relay on somebody's LAN cannot be put into its
// downloader from across that LAN unless the operator says so.
//
// Both addresses are needed. The local one names the link the connection was
// accepted on, which is what an earlier version relied on alone — but lwIP
// accepts a packet for any of its addresses on any interface, so a LAN host
// that routes the access point's address through the node's station address
// arrives at the access point's address from the wrong side. The remote
// address has to belong to that link's own subnet as well. And the remote
// address alone was never enough either: a LAN that happens to be numbered
// like the access point looked local from where it sat.
bool requestIsHostFacing(uint32_t localIpHostOrder, uint32_t remoteIpHostOrder);

// An IPAddress as a host-order number, for comparing with address().
uint32_t hostOrder(const IPAddress& a);

// --- settings ---------------------------------------------------------------
// One table binding the API key, the link type and the stored switch. Four
// handlers used to spell this mapping out separately, and adding a link
// meant four edits with the fourth the one that got missed.
struct Field {
  const char*        key;        // "wifi", "usb", "ppp" — the JSON and export key
  Type               type;
  bool LinkSettings::*on;        // the switch in LinkSettings
};
const Field* fields(size_t& count);

// The switch as the API reports and exports it: the stored flag for a link
// this build can run, false for one it cannot. A default of "on" for a
// driver that does not exist yet is invisible until it does, and a restore
// onto a build that has it gets that build's own default rather than a
// stale "off" written by a node that never had the choice.
bool switchOn(const Link& l, const LinkSettings& s);

// Whether any link this build can run is switched on in these settings.
bool anySwitchOn(const LinkSettings& l);

// What applying a set of link switches came to. One function, because the
// HTTP handler and the console command each used to walk the same steps —
// check, save, ask for the restart, phrase the answer — and the console's
// copy had already lost the lock-out check on the way.
enum class Apply : uint8_t {
  Unchanged,        // nothing differed from what was stored
  Saved,            // saved; no restart needed
  SavedRestarting,  // saved; a restart is armed and will apply it
  SavedNextBoot,    // saved; a restart is already in progress, so it applies at the next boot
  RefusedUnusable,  // a switch was turned on for a link this board or build cannot run
  RefusedLockedOut, // it would leave no way to reach the node
  RefusedBusy,      // a restart is already in progress, so nothing was written
  NvsFailed,
};
// `want` carries the switches to apply; `changed` names which keys were
// given (the rest keep their stored value). `detail` receives the link key
// or reason text a refusal is about.
Apply applyLinks(const LinkSettings& want, const bool* changed, Bootloader_Source source, const char** detail);

// Whether these settings would leave the node with no way in at all: no
// usable link switched on, and the console off. Refused wherever settings are
// written, because a node in that state can only be recovered by erasing it.
bool lockedOut(const LinkSettings& l, bool consoleEnabled);

// --- the links this firmware drives -----------------------------------------
// One body for all of them. They differ in the carrier probe, where the
// address comes from and how the switch is read; the phase machine and what
// it reports are the same, and a phase-machine fix made three times is a
// phase-machine fix made once.
class MachineLink : public Link {
public:
  bool hardware() const override { return true; }
  bool firmware() const override { return true; }
  void begin() override;
  void poll(uint32_t nowMs) override;
  Snapshot snapshot() const override;
  uint32_t address() const override;
  uint32_t netmask() const override;
protected:
  virtual bool       carrier() const = 0;
  virtual IPAddress  ip() const = 0;
  virtual IPAddress  mask() const = 0;
  virtual Addressing addressing() const = 0;
  virtual bool       clientsKnown() const { return false; }
  virtual uint8_t    clients() const { return 0; }
  virtual void       drive() {}                 // a driver of its own to move along before the machine looks
private:
  Machine _m;
};

// The Wi-Fi adapters. WifiManager still owns the radio; these only observe it.
class WifiLink : public MachineLink {
public:
  // The switch as it was applied at boot, not as it is stored now. A Wi-Fi
  // change takes effect at the restart that follows the save; for the second
  // and a half in between, the access point is still on the air and reading
  // the fresh setting reported it gone — which mis-answered the trust rule
  // for the very client that had just saved it.
  bool enabled() const override { return _applied; }
  void begin() override { _applied = wanted(); MachineLink::begin(); }
protected:
  virtual bool wanted() const = 0;              // the stored switch, read once at begin()
private:
  bool _applied = false;
};

class WifiApLink : public WifiLink {
public:
  Type type() const override { return Type::WifiAp; }
  const char* name() const override { return "wifi-ap"; }
protected:
  bool       wanted() const override;
  bool       carrier() const override;
  IPAddress  ip() const override;
  IPAddress  mask() const override;
  Addressing addressing() const override { return Addressing::Static; }
  bool       clientsKnown() const override { return true; }
  uint8_t    clients() const override;
};

class WifiStaLink : public WifiLink {
public:
  Type type() const override { return Type::WifiSta; }
  const char* name() const override { return "wifi-sta"; }
protected:
  bool       wanted() const override;
  bool       carrier() const override;
  IPAddress  ip() const override;
  IPAddress  mask() const override;
  Addressing addressing() const override { return Addressing::Dhcp; }
};

// Links this board could carry but this build does not: they appear in the
// registry so the API lists them with hardware=true, firmware=false and a
// reason, and the settings page can show the switch greyed out rather than
// absent. Nothing else about them runs.
// The S3's USB network link: CDC-NCM on the composite device (UsbNcm.h).
// The device itself is composed before setup() runs; what this link brings up
// and down is the network interface behind it, so the switch applies live —
// no restart, unlike Wi-Fi.
#if HAS_USB_NCM
class UsbNcmLink : public MachineLink {
public:
  Type type() const override { return Type::UsbNcm; }
  const char* name() const override { return "usb0"; }
  bool enabled() const override;                // the stored switch, live
  void begin() override;
protected:
  void       drive() override;                  // the driver follows the switch every pass
  bool       carrier() const override;
  IPAddress  ip() const override;
  IPAddress  mask() const override;
  Addressing addressing() const override { return Addressing::Static; }
  bool       clientsKnown() const override { return true; }
  uint8_t    clients() const override { return 1; }   // one host per cable: whether it is there
};
#endif

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
  uint32_t address() const override { return 0; }
  uint32_t netmask() const override { return 0; }
  const char* reason() const override { return _reason; }
private:
  Type        _type;
  const char* _name;
  bool        _hardware;
  const char* _reason;
};

} // namespace LocalLink
