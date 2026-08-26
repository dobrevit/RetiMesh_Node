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
//  Settings.cpp — see Settings.h
// ============================================================================
#include "Settings.h"

Settings settings;

static const float kBandwidths[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };

bool Settings::validBandwidth(float khz) {
  for (float b : kBandwidths) if (fabsf(b - khz) < 0.05f) return true;
  return false;
}

const char* Settings::securityName(ApSecurity s) {
  switch (s) {
    case ApSecurity::WPA2:      return "wpa2";
    case ApSecurity::WPA2_WPA3: return "wpa2wpa3";
    case ApSecurity::WPA3:      return "wpa3";
    default:                    return "open";
  }
}

bool Settings::securityFromName(const char* name, ApSecurity& out) {
  if (!name) return false;
  if (!strcmp(name, "open"))     { out = ApSecurity::Open;      return true; }
  if (!strcmp(name, "wpa2"))     { out = ApSecurity::WPA2;      return true; }
  if (!strcmp(name, "wpa2wpa3")) { out = ApSecurity::WPA2_WPA3; return true; }
  if (!strcmp(name, "wpa3"))     { out = ApSecurity::WPA3;      return true; }
  return false;
}

void Settings::load() {
  _prefs.begin(NVS_NAMESPACE, false);

  // Preferences logs an error for every read of a missing key, so every
  // read is guarded: defaults (from Config.h) stay in place until a key
  // has actually been written.
  #define LOAD(field, key, getter) if (_prefs.isKey(key)) field = _prefs.getter
  LOAD(_radio.freqMhz,  "r_freq", getFloat ("r_freq"));
  LOAD(_radio.bwKhz,    "r_bw",   getFloat ("r_bw"));
  LOAD(_radio.sf,       "r_sf",   getUChar ("r_sf"));
  LOAD(_radio.cr,       "r_cr",   getUChar ("r_cr"));
  LOAD(_radio.txDbm,    "r_pwr",  getChar  ("r_pwr"));
  LOAD(_radio.syncWord, "r_sync", getUChar ("r_sync"));
  LOAD(_radio.preamble, "r_pre",  getUShort("r_pre"));
  LOAD(_radio.beaconInterval, "r_bcn", getUShort("r_bcn"));
  LOAD(_radio.announceInterval, "r_ann", getUShort("r_ann"));
  if (_prefs.isKey("r_call")) _prefs.getString("r_call", _radio.callsign, sizeof(_radio.callsign));

  if (_prefs.isKey("w_ssid")) _prefs.getString("w_ssid", _wifi.ssid, sizeof(_wifi.ssid));
  if (_prefs.isKey("w_pass")) _prefs.getString("w_pass", _wifi.password, sizeof(_wifi.password));
  if (_prefs.isKey("w_sec"))  _wifi.security = (ApSecurity)_prefs.getUChar("w_sec");
  LOAD(_wifi.channel,     "w_chan", getUChar("w_chan"));
  LOAD(_wifi.maxStations, "w_max",  getUChar("w_max"));
  LOAD(_wifi.hidden,      "w_hid",  getBool ("w_hid"));
  if (_prefs.isKey("w_sta"))  _prefs.getString("w_sta",  _wifi.staSsid, sizeof(_wifi.staSsid));
  if (_prefs.isKey("w_stap")) _prefs.getString("w_stap", _wifi.staPassword, sizeof(_wifi.staPassword));

  if (_prefs.isKey("a_pass")) _prefs.getString("a_pass", _admin.password, sizeof(_admin.password));
  LOAD(_transport.enabled,  "t_en",    getBool ("t_en"));
  LOAD(_transport.loraMode, "t_lmode", getUChar("t_lmode"));
  LOAD(_transport.wifiMode, "t_wmode", getUChar("t_wmode"));
  LOAD(_transport.announceCap,         "t_acap",  getUChar ("t_acap"));
  LOAD(_transport.announceRateTarget,  "t_art",   getUShort("t_art"));
  LOAD(_transport.announceRateGrace,   "t_arg",   getUChar ("t_arg"));
  LOAD(_transport.announceRatePenalty, "t_arp",   getUShort("t_arp"));
  LOAD(_transport.autoEnabled,         "t_auto",  getBool  ("t_auto"));
  LOAD(_transport.powerProfile,        "t_pwr",   getUChar ("t_pwr"));
  if (_prefs.isKey("t_agrp")) _prefs.getString("t_agrp", _transport.autoGroupId, sizeof(_transport.autoGroupId));
  if (_admin.password[0] == '\0') strlcpy(_admin.password, ADMIN_PASSWORD_DEFAULT, sizeof(_admin.password));
  #undef LOAD

  log_i("settings loaded: radio %.3f MHz BW %.1f SF%d CR%d %d dBm sync 0x%02X; ap '%s' %s ch%d",
        _radio.freqMhz, _radio.bwKhz, _radio.sf, _radio.cr, _radio.txDbm, _radio.syncWord,
        _wifi.ssid[0] ? _wifi.ssid : "(auto)", securityName(_wifi.security), _wifi.channel);
}

bool Settings::saveRadio(const RadioSettings& r) {
  _radio = r;
  bool ok = _prefs.putFloat ("r_freq", r.freqMhz)  > 0
         && _prefs.putFloat ("r_bw",   r.bwKhz)    > 0
         && _prefs.putUChar ("r_sf",   r.sf)       > 0
         && _prefs.putUChar ("r_cr",   r.cr)       > 0
         && _prefs.putChar  ("r_pwr",  r.txDbm)    > 0
         && _prefs.putUChar ("r_sync", r.syncWord) > 0
         && _prefs.putUShort("r_pre",  r.preamble) > 0
         && _prefs.putUShort("r_bcn",  r.beaconInterval) > 0
         && _prefs.putUShort("r_ann",  r.announceInterval) > 0
         && _prefs.putString("r_call", r.callsign) >= 0;
  if (!ok) log_e("NVS write failed (radio)");
  return ok;
}

bool Settings::saveWifi(const WifiSettings& w) {
  _wifi = w;
  // putString returns 0 for an empty string, so check those separately.
  bool ok = _prefs.putString("w_ssid", w.ssid)     >= 0
         && _prefs.putString("w_pass", w.password) >= 0
         && _prefs.putUChar ("w_sec",  (uint8_t)w.security) > 0
         && _prefs.putUChar ("w_chan", w.channel)  > 0
         && _prefs.putUChar ("w_max",  w.maxStations) > 0
         && _prefs.putBool  ("w_hid",  w.hidden)   > 0
         && _prefs.putString("w_sta",  w.staSsid)  >= 0
         && _prefs.putString("w_stap", w.staPassword) >= 0;
  if (!ok) log_e("NVS write failed (wifi)");
  return ok;
}

bool Settings::saveAdminPassword(const char* password) {
  strlcpy(_admin.password, password, sizeof(_admin.password));
  bool ok = _prefs.putString("a_pass", _admin.password) > 0;
  if (!ok) log_e("NVS write failed (admin)");
  return ok;
}

bool Settings::saveTransport(const TransportSettings& t) {
  _transport = t;
  bool ok = _prefs.putBool ("t_en",    t.enabled)  > 0
         && _prefs.putUChar("t_lmode", t.loraMode) > 0
         && _prefs.putUChar("t_wmode", t.wifiMode) > 0
         && _prefs.putUChar ("t_acap",  t.announceCap) > 0
         && _prefs.putUShort("t_art",   t.announceRateTarget) > 0
         && _prefs.putUChar ("t_arg",   t.announceRateGrace) > 0
         && _prefs.putUShort("t_arp",   t.announceRatePenalty) > 0
         && _prefs.putBool  ("t_auto",  t.autoEnabled) > 0
         && _prefs.putUChar ("t_pwr",   t.powerProfile) > 0
         && _prefs.putString("t_agrp",  t.autoGroupId) >= 0;
  if (!ok) log_e("NVS write failed (transport)");
  return ok;
}

void Settings::factoryReset() {
  _prefs.clear();
  _radio = RadioSettings();
  _wifi  = WifiSettings();
  _admin = AdminSettings();
  _transport = TransportSettings();
  log_w("settings: factory reset");
}
