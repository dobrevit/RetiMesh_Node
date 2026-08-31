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
//  SettingsFields.h — every setting, by name, for the console
//
//  The web API takes settings a section at a time as JSON. The console needs
//  them one at a time by name, because it is used when the web API cannot be
//  reached at all: a node whose Wi-Fi password is wrong, or whose announce
//  interval is zero, is reachable over the cable and nowhere else. That is
//  not a hypothetical — a Heltec Wireless Stick spent an afternoon in exactly
//  that state, answering its console perfectly while no network path to it
//  existed, and the only fix available was to erase the whole node.
//
//  So: a table of keys, each of which can be rendered and assigned. The names
//  are the API's JSON names with their section in front — `radio.sf`,
//  `wifi.sta_ssid`, `links.ppp_baud` — so an operator who knows one knows the
//  other. Assignment validates through SettingsRules, which is where the web
//  API's bounds live too, and refuses with the same words.
//
//  No authentication, deliberately. The console is the serial port: whoever
//  has it can already dump the flash, reflash the board, and ask for the ROM
//  downloader with BOOTLOADER CONFIRM, which this node has always allowed.
//  A password on the settings would guard a window beside an open door.
//
//  Secrets render as (set) or (unset) rather than as themselves. Physical
//  access implies them, but the console shares its port with the log, and a
//  password that has been printed once is a password in somebody's scrollback.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

struct MaintenanceSettings;

namespace SettingsFields {

enum class Result : uint8_t {
  Ok,          // saved, and applied where it applies live
  OkRestart,   // saved; it takes effect at a restart, which has been asked for
  OkNextBoot,  // saved; a restart is already in progress, so it lands at the next boot
  Unknown,     // no such key
  BadValue,    // refused by the rule for that setting; `err` says why
  Refused,     // would leave the node unreachable
  Unsupported, // this board or build has no such link
  Busy,        // a restart is already in progress, so nothing was written
  NvsFailed,   // the store would not take it
};

// The table, in the order a person would want to read it.
size_t count();
const char* keyAt(size_t i);

// "<key>=<value>", one console data line. False for an index or key that is
// not there.
bool render(size_t i, char* out, size_t len);
bool renderKey(const char* key, char* out, size_t len);

// Whether `prefix` names a section that exists ("radio", "wifi", ...), so the
// console can tell an empty section from a misspelt one.
bool sectionExists(const char* prefix);
bool keyInSection(size_t i, const char* prefix);

// Parse, validate and apply one setting. `err` carries the refusal.
Result set(const char* key, const char* value, char* err, size_t errLen);

// The maintenance section's commit, exported because the web API writes the
// same section and must refuse on the same terms and ask for the same
// restart. The rule for whether a change leaves a way into the node, and for
// what the portal's switch costs, has one home and this is it.
Result commitMaintenance(MaintenanceSettings& m, char* err, size_t n);

// Puts the remote-administration list into `m`, or says why it will not. Both
// the console's SET and the HTTP endpoint ask this rather than each checking
// for itself: they had a copy each and the copies already disagreed — "-"
// cleared the list at the console and was a 400 over HTTP — and the next rule
// added to one would silently not apply to the other.
bool setRnsAdmins(MaintenanceSettings& m, const char* value, char* err, size_t n);

// The words for a result, for the console and anything else that reports one.
const char* resultText(Result r);

} // namespace SettingsFields
