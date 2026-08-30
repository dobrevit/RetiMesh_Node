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

// SettingsFields.cpp — see SettingsFields.h. A table of keys, and one commit
// function per section that validates through SettingsRules and applies the
// section the same way its HTTP handler does.
#include "SettingsFields.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "Settings.h"
#include "SettingsRules.h"
#include "LocalLink.h"
#include "LoRaRadio.h"
#include "Bootloader.h"
#include "Gps.h"
#include "Power.h"

extern Settings settings;
extern LoRaRadio loraRadio;

namespace SettingsFields {
namespace {

// ---------------------------------------------------------------------------
// Parsing. A value arrives as one console token; these say whether it is one.
// ---------------------------------------------------------------------------
bool parseBool(const char* v, bool& out) {
  if (!strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcmp(v, "1"))  { out = true;  return true; }
  if (!strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcmp(v, "0")) { out = false; return true; }
  return false;
}

bool parseU32(const char* v, uint32_t& out) {
  char* end = nullptr;
  const unsigned long n = strtoul(v, &end, 10);
  if (end == v || *end) return false;
  out = (uint32_t)n;
  return true;
}

bool parseI32(const char* v, int32_t& out) {
  char* end = nullptr;
  const long n = strtol(v, &end, 10);
  if (end == v || *end) return false;
  out = (int32_t)n;
  return true;
}

bool parseFloat(const char* v, float& out) {
  char* end = nullptr;
  const float f = strtof(v, &end);
  if (end == v || *end) return false;
  // "nan" parses cleanly and then passes every bound, because each comparison
  // against a NaN is false: it would be stored and handed to the radio as a
  // frequency no probe can set. "inf" is caught by the upper bound; this is
  // the one that slips past.
  if (!isfinite(f)) return false;
  out = f;
  return true;
}

// An unsigned value that has to fit the field it is going into. Without the
// ceiling the cast wraps and the stored value is not the one that was typed:
// preamble 65542 becomes 6, which then passes its own 6-1000 bound.
bool parseU32Max(const char* v, uint32_t max, uint32_t& out, char* err, size_t n) {
  if (!parseU32(v, out)) { snprintf(err, n, "expected a whole number"); return false; }
  if (out > max) { snprintf(err, n, "value out of range: at most %lu here", (unsigned long)max); return false; }
  return true;
}

bool parseI32Range(const char* v, int32_t lo, int32_t hi, int32_t& out, char* err, size_t n) {
  if (!parseI32(v, out)) { snprintf(err, n, "expected a whole number"); return false; }
  if (out < lo || out > hi) { snprintf(err, n, "value out of range: %ld to %ld here", (long)lo, (long)hi); return false; }
  return true;
}

// ---------------------------------------------------------------------------
// Commit. One per section, doing exactly what that section's HTTP handler
// does after its own parse: validate against the shared rule, save, and apply
// whatever applies without a restart.
// ---------------------------------------------------------------------------
// Nothing is written while a restart is on its way. Every settings POST is
// refused with 503 in that state and LocalLink::applyLinks refuses with
// RefusedBusy for the reason it gives: a setting saved now might not reach
// flash before the restart does.
bool restartPending(char* err, size_t n) {
  if (!Bootloader::pending()) return false;
  snprintf(err, n, "a restart is already in progress");
  return true;
}

Result commitRadio(RadioSettings& r, char* err, size_t n) {
  if (restartPending(err, n)) return Result::Busy;
  const int8_t maxDbm = loraRadio.online() ? loraRadio.maxTxDbm() : 22;
  if (!SettingsRules::validateRadio(r, loraRadio.caps(), maxDbm, err, n)) return Result::BadValue;
  if (!settings.saveRadio(r)) return Result::NvsFailed;
  if (loraRadio.online()) loraRadio.requestReconfigure(r);
  #if HAS_GPS
    Gps::setEnabled(r.gpsEnabled);
  #endif
  return Result::Ok;
}

// Wi-Fi is applied at a restart: the access point cannot be torn down under
// the request that changed it, which is the same reason the web API answers
// "restart": true rather than switching the radio there and then.
Result commitWifi(WifiSettings& w, char* err, size_t n) {
  if (restartPending(err, n)) return Result::Busy;
  if (!SettingsRules::validateWifi(w, err, n)) return Result::BadValue;
  if (!settings.saveWifi(w)) return Result::NvsFailed;
  return Bootloader::reboot() ? Result::OkRestart : Result::OkNextBoot;
}

// The interface modes are registered with Transport at boot, so they need a
// restart too; the power profile and the store's home are read at boot as well.
Result commitTransport(TransportSettings& t, char* err, size_t n) {
  if (restartPending(err, n)) return Result::Busy;
  if (!SettingsRules::validateTransport(t, err, n)) return Result::BadValue;
  const TransportSettings before = settings.transport();
  if (!settings.saveTransport(t)) return Result::NvsFailed;
  Power::apply((Power::Profile)t.powerProfile);              // live, as the web API applies it
  // Only the fields the interfaces are built from need a restart; the rest —
  // the announce rate limits, the power profile — take effect as they are
  // saved. Restarting for all of them would drop every client and this very
  // console session to change a rate limit.
  const bool needRestart = before.enabled != t.enabled || before.loraMode != t.loraMode ||
                           before.wifiMode != t.wifiMode || before.autoMode != t.autoMode ||
                           before.autoEnabled != t.autoEnabled ||
                           strcmp(before.autoGroupId, t.autoGroupId) != 0 ||
                           before.announceCap != t.announceCap;
  if (!needRestart) return Result::Ok;
  return Bootloader::reboot() ? Result::OkRestart : Result::OkNextBoot;
}

Result commitMaintenance(MaintenanceSettings& m, char* err, size_t n) {
  if (restartPending(err, n)) return Result::Busy;
  // The console can switch itself off, but not into a node with no way in.
  if (LocalLink::lockedOut(settings.links(), m.consoleEnabled)) {
    snprintf(err, n, "refused: no local link is enabled, so switching the console off "
                     "would leave no way to reach the node");
    return Result::Refused;
  }
  if (!settings.saveMaintenance(m)) return Result::NvsFailed;
  return Result::Ok;
}

Result commitAdmin(const char* password, char* err, size_t n) {
  if (restartPending(err, n)) return Result::Busy;
  if (!SettingsRules::validateAdminPassword(password, err, n)) return Result::BadValue;
  if (!settings.saveAdminPassword(password)) return Result::NvsFailed;
  return Result::Ok;
}

// Links go through LocalLink::applyLinks, which is already the one place that
// decides whether a switch may be thrown: it refuses a link this board or
// build cannot run, refuses a combination that locks the operator out, and
// asks for the restart when one is needed.
Result commitLinks(const LinkSettings& want, const bool* changed, char* err, size_t n) {
  const char* detail = nullptr;
  switch (LocalLink::applyLinks(want, changed, Bootloader::Source::Console, &detail)) {
    case LocalLink::Apply::Unchanged:
    case LocalLink::Apply::Saved:            return Result::Ok;
    case LocalLink::Apply::SavedRestarting:  return Result::OkRestart;
    case LocalLink::Apply::SavedNextBoot:    return Result::OkNextBoot;
    case LocalLink::Apply::RefusedUnusable:
      snprintf(err, n, "%s", detail ? detail : "this board or build has no such link");
      return Result::Unsupported;
    case LocalLink::Apply::RefusedLockedOut:
    case LocalLink::Apply::RefusedBaud:
      snprintf(err, n, "%s", detail ? detail : "refused");
      return Result::Refused;
    case LocalLink::Apply::RefusedBusy:
      snprintf(err, n, "a restart is already in progress");
      return Result::Busy;
    default:                                 return Result::NvsFailed;
  }
}

// A link switch by name, so the four of them are one entry each rather than
// four copies of the same six lines.
Result setLinkSwitch(bool LinkSettings::*member, const char* v, char* err, size_t n) {
  bool on = false;
  if (!parseBool(v, on)) { snprintf(err, n, "expected on or off"); return Result::BadValue; }
  LinkSettings want = settings.links();
  want.*member = on;
  size_t fields = 0;
  const LocalLink::Field* f = LocalLink::fields(fields);
  bool changed[8] = {false};
  for (size_t i = 0; i < fields && i < 8; i++) changed[i] = (f[i].on == member);
  return commitLinks(want, changed, err, n);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------
struct Entry {
  const char* key;
  void   (*render)(char* out, size_t len);
  Result (*assign)(const char* value, char* err, size_t errLen);
};

// A secret is reported as whether it is set, never as itself: the console
// shares its port with the log (SettingsFields.h).
void renderSecret(char* out, size_t len, const char* v) { snprintf(out, len, "%s", v[0] ? "(set)" : "(unset)"); }

// Text is quoted, always. An empty value would otherwise render as "key=" with
// nothing after it — which reads as a missing key rather than an empty one —
// and an SSID with a space in it would split across two key-value pairs in
// anything parsing the line.
void renderStr(char* out, size_t len, const char* v) { snprintf(out, len, "\"%s\"", v); }

const Entry kFields[] = {
  // --- radio -------------------------------------------------------------
  { "radio.region",
    [](char* o, size_t n) { renderStr(o, n, settings.radio().region); },
    [](const char* v, char* e, size_t n) { RadioSettings r = settings.radio();
      strlcpy(r.region, v, sizeof(r.region)); return commitRadio(r, e, n); } },
  { "radio.freq_mhz",
    [](char* o, size_t n) { snprintf(o, n, "%.3f", (double)settings.radio().freqMhz); },
    [](const char* v, char* e, size_t n) { float f; if (!parseFloat(v, f)) { snprintf(e, n, "expected a number in MHz"); return Result::BadValue; }
      RadioSettings r = settings.radio(); r.freqMhz = f; return commitRadio(r, e, n); } },
  { "radio.bw_khz",
    [](char* o, size_t n) { snprintf(o, n, "%.1f", (double)settings.radio().bwKhz); },
    [](const char* v, char* e, size_t n) { float f; if (!parseFloat(v, f)) { snprintf(e, n, "expected a bandwidth in kHz"); return Result::BadValue; }
      RadioSettings r = settings.radio(); r.bwKhz = f; return commitRadio(r, e, n); } },
  { "radio.sf",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().sf); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.sf = (uint8_t)u; return commitRadio(r, e, n); } },
  { "radio.cr",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().cr); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.cr = (uint8_t)u; return commitRadio(r, e, n); } },
  { "radio.tx_dbm",
    [](char* o, size_t n) { snprintf(o, n, "%d", (int)settings.radio().txDbm); },
    [](const char* v, char* e, size_t n) { int32_t i; if (!parseI32Range(v, -128, 127, i, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.txDbm = (int8_t)i; return commitRadio(r, e, n); } },
  { "radio.sync_word",
    [](char* o, size_t n) { snprintf(o, n, "0x%02X", (unsigned)settings.radio().syncWord); },
    [](const char* v, char* e, size_t n) { char* end = nullptr; const unsigned long u = strtoul(v, &end, 0);
      if (end == v || *end || u > 255) { snprintf(e, n, "expected a sync word 0-255, decimal or 0x hex"); return Result::BadValue; }
      RadioSettings r = settings.radio(); r.syncWord = (uint8_t)u; return commitRadio(r, e, n); } },
  { "radio.preamble",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().preamble); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 65535, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.preamble = (uint16_t)u; return commitRadio(r, e, n); } },
  { "radio.beacon_interval",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().beaconInterval); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 65535, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.beaconInterval = (uint16_t)u; return commitRadio(r, e, n); } },
  { "radio.announce_interval",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().announceInterval); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 65535, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.announceInterval = (uint16_t)u; return commitRadio(r, e, n); } },
  { "radio.duty_cycle_pct",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.radio().dutyCyclePct); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      RadioSettings r = settings.radio(); r.dutyCyclePct = (uint8_t)u; return commitRadio(r, e, n); } },
  { "radio.callsign",
    [](char* o, size_t n) { renderStr(o, n, settings.radio().callsign); },
    [](const char* v, char* e, size_t n) { RadioSettings r = settings.radio();
      // Checked before the copy: the field is fixed-width, so afterwards the
      // overlong value is already a truncated one that passes.
      if (!SettingsRules::validateCallsign(v, e, n)) return Result::BadValue;
      strlcpy(r.callsign, v, sizeof(r.callsign)); return commitRadio(r, e, n); } },
  { "radio.gps_enabled",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.radio().gpsEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      RadioSettings r = settings.radio(); r.gpsEnabled = b; return commitRadio(r, e, n); } },
  { "radio.gps_share_position",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.radio().gpsSharePosition ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      RadioSettings r = settings.radio(); r.gpsSharePosition = b; return commitRadio(r, e, n); } },

  // --- wifi --------------------------------------------------------------
  { "wifi.ssid",
    [](char* o, size_t n) { renderStr(o, n, settings.wifi().ssid); },
    [](const char* v, char* e, size_t n) { WifiSettings w = settings.wifi();
      if (!SettingsRules::validateSsid(v, false, e, n)) return Result::BadValue;
      strlcpy(w.ssid, v, sizeof(w.ssid)); return commitWifi(w, e, n); } },
  { "wifi.password",
    [](char* o, size_t n) { renderSecret(o, n, settings.wifi().password); },
    [](const char* v, char* e, size_t n) { WifiSettings w = settings.wifi();
      strlcpy(w.password, v, sizeof(w.password)); return commitWifi(w, e, n); } },
  { "wifi.security",
    [](char* o, size_t n) { snprintf(o, n, "%s", Settings::securityName(settings.wifi().security)); },
    [](const char* v, char* e, size_t n) { WifiSettings w = settings.wifi();
      if (!Settings::securityFromName(v, w.security)) { snprintf(e, n, "security must be open|wpa2|wpa2wpa3|wpa3"); return Result::BadValue; }
      return commitWifi(w, e, n); } },
  { "wifi.channel",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.wifi().channel); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      WifiSettings w = settings.wifi(); w.channel = (uint8_t)u; return commitWifi(w, e, n); } },
  { "wifi.max_stations",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.wifi().maxStations); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      WifiSettings w = settings.wifi(); w.maxStations = (uint8_t)u; return commitWifi(w, e, n); } },
  { "wifi.hidden",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.wifi().hidden ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      WifiSettings w = settings.wifi(); w.hidden = b; return commitWifi(w, e, n); } },
  { "wifi.sta_ssid",
    [](char* o, size_t n) { renderStr(o, n, settings.wifi().staSsid); },
    [](const char* v, char* e, size_t n) { WifiSettings w = settings.wifi();
      // The station's password goes with its network: leaving the old one
      // against a new SSID is how a node ends up in an AUTH_FAIL loop.
      if (!SettingsRules::validateSsid(v, true, e, n)) return Result::BadValue;
      strlcpy(w.staSsid, v, sizeof(w.staSsid));
      if (!w.staSsid[0]) w.staPassword[0] = '\0';
      return commitWifi(w, e, n); } },
  { "wifi.sta_password",
    [](char* o, size_t n) { renderSecret(o, n, settings.wifi().staPassword); },
    [](const char* v, char* e, size_t n) { WifiSettings w = settings.wifi();
      strlcpy(w.staPassword, v, sizeof(w.staPassword)); return commitWifi(w, e, n); } },

  // --- links -------------------------------------------------------------
  { "links.wifi",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.links().wifiEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { return setLinkSwitch(&LinkSettings::wifiEnabled, v, e, n); } },
  { "links.usb",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.links().usbEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { return setLinkSwitch(&LinkSettings::usbEnabled, v, e, n); } },
  { "links.ppp",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.links().pppEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { return setLinkSwitch(&LinkSettings::pppEnabled, v, e, n); } },
  { "links.ppp_baud",
    [](char* o, size_t n) { snprintf(o, n, "%lu", (unsigned long)settings.links().pppBaud); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32(v, u)) { snprintf(e, n, "expected a baud rate"); return Result::BadValue; }
      LinkSettings want = settings.links(); want.pppBaud = u;
      bool changed[8] = {false};       // the baud travels with the switches, unchanged
      return commitLinks(want, changed, e, n); } },

  // --- maintenance -------------------------------------------------------
  { "maintenance.console_enabled",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.maintenance().consoleEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      MaintenanceSettings m = settings.maintenance(); m.consoleEnabled = b; return commitMaintenance(m, e, n); } },
  { "maintenance.console_tcp",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.maintenance().consoleTcp ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      MaintenanceSettings m = settings.maintenance(); m.consoleTcp = b; return commitMaintenance(m, e, n); } },
  { "maintenance.bootloader_api",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.maintenance().bootloaderApi ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      MaintenanceSettings m = settings.maintenance(); m.bootloaderApi = b; return commitMaintenance(m, e, n); } },
  { "maintenance.bootloader_from_lan",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.maintenance().bootloaderFromLan ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      MaintenanceSettings m = settings.maintenance(); m.bootloaderFromLan = b; return commitMaintenance(m, e, n); } },

  // --- transport ---------------------------------------------------------
  { "transport.enabled",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.transport().enabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      TransportSettings t = settings.transport(); t.enabled = b; return commitTransport(t, e, n); } },
  { "transport.lora_mode",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().loraMode); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.loraMode = (uint8_t)u; return commitTransport(t, e, n); } },
  { "transport.wifi_mode",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().wifiMode); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.wifiMode = (uint8_t)u; return commitTransport(t, e, n); } },
  { "transport.auto_mode",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().autoMode); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.autoMode = (uint8_t)u; return commitTransport(t, e, n); } },
  { "transport.auto_enabled",
    [](char* o, size_t n) { snprintf(o, n, "%s", settings.transport().autoEnabled ? "on" : "off"); },
    [](const char* v, char* e, size_t n) { bool b; if (!parseBool(v, b)) { snprintf(e, n, "expected on or off"); return Result::BadValue; }
      TransportSettings t = settings.transport(); t.autoEnabled = b; return commitTransport(t, e, n); } },
  { "transport.auto_group_id",
    [](char* o, size_t n) { renderStr(o, n, settings.transport().autoGroupId); },
    [](const char* v, char* e, size_t n) { TransportSettings t = settings.transport();
      if (!SettingsRules::validateGroupId(v, e, n)) return Result::BadValue;
      strlcpy(t.autoGroupId, v, sizeof(t.autoGroupId)); return commitTransport(t, e, n); } },
  { "transport.announce_cap",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().announceCap); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.announceCap = (uint8_t)u; return commitTransport(t, e, n); } },
  { "transport.announce_rate_target",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().announceRateTarget); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 65535, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.announceRateTarget = (uint16_t)u; return commitTransport(t, e, n); } },
  { "transport.announce_rate_grace",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().announceRateGrace); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 255, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.announceRateGrace = (uint8_t)u; return commitTransport(t, e, n); } },
  { "transport.announce_rate_penalty",
    [](char* o, size_t n) { snprintf(o, n, "%u", (unsigned)settings.transport().announceRatePenalty); },
    [](const char* v, char* e, size_t n) { uint32_t u; if (!parseU32Max(v, 65535, u, e, n)) return Result::BadValue;
      TransportSettings t = settings.transport(); t.announceRatePenalty = (uint16_t)u; return commitTransport(t, e, n); } },
  { "transport.power_profile",
    // By name, as the web API takes it: "2" means nothing to a person reading
    // a settings dump, and the two interfaces should not disagree about what
    // this value looks like.
    [](char* o, size_t n) { snprintf(o, n, "%s", Power::profileName((Power::Profile)settings.transport().powerProfile)); },
    [](const char* v, char* e, size_t n) { Power::Profile pp;
      if (!Power::profileFromName(v, pp)) { snprintf(e, n, "power_profile must be performance|balanced|battery"); return Result::BadValue; }
      TransportSettings t = settings.transport(); t.powerProfile = (uint8_t)pp; return commitTransport(t, e, n); } },

  // --- admin -------------------------------------------------------------
  { "admin.password",
    [](char* o, size_t n) { renderSecret(o, n, settings.admin().password); },
    [](const char* v, char* e, size_t n) { return commitAdmin(v, e, n); } },
};

constexpr size_t kCount = sizeof(kFields) / sizeof(kFields[0]);

} // namespace

size_t count() { return kCount; }
const char* keyAt(size_t i) { return i < kCount ? kFields[i].key : nullptr; }

bool render(size_t i, char* out, size_t len) {
  if (i >= kCount) return false;
  char value[96] = "";
  kFields[i].render(value, sizeof(value));
  snprintf(out, len, "%s=%s", kFields[i].key, value);
  return true;
}

bool renderKey(const char* key, char* out, size_t len) {
  for (size_t i = 0; i < kCount; i++)
    if (!strcasecmp(kFields[i].key, key)) return render(i, out, len);
  return false;
}

bool keyInSection(size_t i, const char* prefix) {
  if (i >= kCount) return false;
  const size_t p = strlen(prefix);
  return !strncasecmp(kFields[i].key, prefix, p) && kFields[i].key[p] == '.';
}

bool sectionExists(const char* prefix) {
  for (size_t i = 0; i < kCount; i++) if (keyInSection(i, prefix)) return true;
  return false;
}

Result set(const char* key, const char* value, char* err, size_t errLen) {
  for (size_t i = 0; i < kCount; i++)
    if (!strcasecmp(kFields[i].key, key)) return kFields[i].assign(value, err, errLen);
  return Result::Unknown;
}

const char* resultText(Result r) {
  switch (r) {
    case Result::Ok:         return "saved";
    case Result::OkRestart:  return "saved; restarting to apply it";
    case Result::OkNextBoot: return "saved; a restart is already in progress, so it applies at the next boot";
    case Result::Unknown:     return "no such setting";
    case Result::BadValue:    return "bad value";
    case Result::Refused:     return "refused";
    case Result::Unsupported: return "not on this board";
    case Result::Busy:        return "a restart is already in progress";
    case Result::NvsFailed:  return "the settings store would not take it";
  }
  return "unknown";
}

} // namespace SettingsFields
