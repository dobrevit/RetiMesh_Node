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
//  SettingsRules.h — what a settings value may be, in one place
//
//  Every bound a setting has used to live inside the HTTP handler that
//  parsed it, phrased as a call to sendError(). That was fine while the web
//  API was the only way to change a setting. It is not: the console changes
//  settings too, and a node whose Wi-Fi is misconfigured is reachable by
//  nothing else. Two callers enforcing the same rule from two copies is the
//  arrangement where one of them quietly drifts, and the one that drifts is
//  the one nobody exercises.
//
//  So the rules live here, pure: they take the value and what bounds it —
//  the transceiver that is fitted, the region it is operated in — and write
//  the refusal in the operator's words. They touch no settings store, no
//  radio and no request, which is what lets the native tests hold them to
//  the same vectors the firmware uses (test_settings_rules).
//
//  The messages are the API's, verbatim. An operator who reads one over the
//  cable and one over HTTP should not have to wonder whether they mean the
//  same thing.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "Settings.h"
#include "RadioCaps.h"
#include "Airtime.h"

namespace SettingsRules {

// The radio. `caps` is the transceiver actually fitted and `maxDbm` the most
// it will emit — both are asked of the driver rather than assumed, because an
// SX1280 tunes 2400-2500 MHz and has four bandwidths, none of which appear in
// the SX127x list. False with `err` filled in when the value cannot stand.
inline bool validateRadio(const RadioSettings& r, const RadioCaps::Caps& caps,
                          int8_t maxDbm, char* err, size_t errLen) {
  char bwlist[96];
  // The callsign travels as text and is checked as text: printable ASCII
  // without spaces, because it is put into an announce and read back out of
  // one by things that split on whitespace.
  for (const char* p = r.callsign; *p; p++)
    if (*p < 0x21 || *p > 0x7E) {
      snprintf(err, errLen, "callsign: printable ASCII without spaces only");
      return false;
    }
  if (strlen(r.callsign) > 32) {
    snprintf(err, errLen, "callsign must be at most 32 characters");
    return false;
  }

  // The region bounds the channel, and the chip bounds the region. Both have
  // to hold, and the message says which one was missed rather than quoting a
  // range the operator cannot use anyway.
  const Airtime::RegionInfo* region = Airtime::regionByKey(r.region);
  if (!region) {
    snprintf(err, errLen, "unknown region — pick one the node offers");
    return false;
  }
  const float lowMhz  = region->lowMhz  > caps.freqMinMhz ? region->lowMhz  : caps.freqMinMhz;
  const float highMhz = region->highMhz < caps.freqMaxMhz ? region->highMhz : caps.freqMaxMhz;
  if (lowMhz > highMhz) {
    snprintf(err, errLen, "the %s cannot tune %s — choose a region this radio reaches",
             caps.name, region->name);
    return false;
  }
  if (r.freqMhz < lowMhz || r.freqMhz > highMhz) {
    snprintf(err, errLen, "frequency must be %g-%g MHz in %s on the %s",
             (double)lowMhz, (double)highMhz, region->name, caps.name);
    return false;
  }
  if (!RadioCaps::bandwidthSupported(caps, r.bwKhz)) {
    snprintf(err, errLen, "the %s supports these bandwidths in kHz: %s",
             caps.name, RadioCaps::bandwidthList(caps, bwlist, sizeof(bwlist)));
    return false;
  }
  if (r.sf < caps.sfMin || r.sf > caps.sfMax) {
    snprintf(err, errLen, "spreading factor must be %u-%u on the %s",
             (unsigned)caps.sfMin, (unsigned)caps.sfMax, caps.name);
    return false;
  }
  if (r.cr < 5 || r.cr > 8) {
    snprintf(err, errLen, "coding rate must be 5-8 (4/5..4/8)");
    return false;
  }
  if (r.txDbm < caps.txMinDbm || r.txDbm > maxDbm) {
    snprintf(err, errLen, "tx power must be %d to %d dBm on the %s",
             (int)caps.txMinDbm, (int)maxDbm, caps.name);
    return false;
  }
  if (r.preamble < 6 || r.preamble > 1000) {
    snprintf(err, errLen, "preamble must be 6-1000 symbols");
    return false;
  }
  if (r.beaconInterval != 0 && (r.beaconInterval < 10 || r.beaconInterval > 3600)) {
    snprintf(err, errLen, "beacon interval must be 0 (off) or 10-3600 s");
    return false;
  }
  if (r.announceInterval != 0 && (r.announceInterval < 60 || r.announceInterval > 43200)) {
    snprintf(err, errLen, "announce interval must be 0 (off) or 60-43200 s");
    return false;
  }
  if (r.dutyCyclePct > 100) {
    snprintf(err, errLen, "duty cycle must be 0 (off) or 1-100 %%");
    return false;
  }
  return true;
}

// Wi-Fi. The lengths are the 802.11 ones and the rest is what the access point
// can be asked for; the password rule is checked against the security in the
// same set of values, because "open with a short password" and "secured with
// none" are the two ways to end up with an access point nobody can join.
inline bool validateWifi(const WifiSettings& w, char* err, size_t errLen) {
  if (strlen(w.ssid) > 32) {
    snprintf(err, errLen, "ssid must be at most 32 characters");
    return false;
  }
  if (strlen(w.staSsid) > 32) {
    snprintf(err, errLen, "station ssid must be at most 32 characters");
    return false;
  }
  if (w.password[0] && (strlen(w.password) < 8 || strlen(w.password) > 63)) {
    snprintf(err, errLen, "password must be 8-63 characters");
    return false;
  }
  if (strlen(w.staPassword) > 63) {
    snprintf(err, errLen, "station password too long");
    return false;
  }
  if (w.security != ApSecurity::Open && strlen(w.password) < 8) {
    snprintf(err, errLen, "a password is required for a secured network");
    return false;
  }
  if (w.channel < 1 || w.channel > 13) {
    snprintf(err, errLen, "channel must be 1-13");
    return false;
  }
  if (w.maxStations < 1 || w.maxStations > 10) {
    snprintf(err, errLen, "max stations must be 1-10");
    return false;
  }
  return true;
}

} // namespace SettingsRules
