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
//  Settings.h — persistent node configuration (NVS via Preferences)
//
//  Everything the admin can change at runtime lives here: radio channel
//  parameters, the Wi-Fi access point, and the admin password. Defaults
//  come from Config.h; values are stored in the "retimesh" NVS namespace
//  and survive reflashing the app partition (a factory reset or a full
//  erase clears them).
//
//  Writers: the HTTP settings handlers only (AsyncTCP task).
//  Readers: everyone. The structs are small POD copies — the radio task
//  receives its own copy via LoRaRadio::requestReconfigure().
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "Config.h"

enum class ApSecurity : uint8_t { Open = 0, WPA2 = 1, WPA2_WPA3 = 2, WPA3 = 3 };

struct RadioSettings {
  float    freqMhz  = RF_FREQ_MHZ;
  float    bwKhz    = RF_BW_KHZ;
  uint8_t  sf       = RF_SF;
  uint8_t  cr       = RF_CR;            // 5..8 => 4/5 .. 4/8
  int8_t   txDbm    = RF_TX_DBM;
  uint8_t  syncWord = RF_SYNCWORD;
  uint16_t preamble = RF_PREAMBLE_SYMS;
  uint16_t beaconInterval = BEACON_INTERVAL_S;   // s, 0 = off
  uint16_t announceInterval = ANNOUNCE_INTERVAL_S; // s, 0 = off
  char     callsign[33]   = "";                  // "" = use the SSID
};

struct WifiSettings {
  char       ssid[33]     = "";         // empty => AP_SSID_PREFIX-<MAC tail>
  char       password[65] = AP_PASSWORD;
  ApSecurity security     = (sizeof(AP_PASSWORD) - 1 >= 8) ? (ApSecurity)AP_SECURITY_DEFAULT
                                                           : ApSecurity::Open;
  uint8_t    channel      = AP_CHANNEL;
  uint8_t    maxStations  = AP_MAX_STATIONS;
  bool       hidden       = false;
  // Station mode: also join an existing Wi-Fi network (AP stays up).
  char       staSsid[33]     = "";      // "" = station mode off
  char       staPassword[65] = "";
};

// Interface modes use rnsd's vocabulary: 1 full, 2 gateway, 3 access_point,
// 4 roaming, 5 boundary. Changing these needs a restart (interfaces are
// registered with Transport at boot), same as editing rnsd's config.
struct TransportSettings {
  bool     enabled  = true;
  uint8_t  loraMode = 1;                // full: the LoRa channel is the mesh
  uint8_t  wifiMode = 3;                // access_point: phones come and go
  // rnsd's announce_cap / announce_rate_target / announce_rate_grace /
  // announce_rate_penalty, applied to every interface.
  uint8_t  announceCap        = 2;      // % of interface bandwidth for announces
  uint16_t announceRateTarget = 0;      // s between announces from one destination (0 = off)
  uint8_t  announceRateGrace  = 0;      // violations tolerated before blocking
  uint16_t announceRatePenalty = 0;     // s added to the block
  bool     autoEnabled = true;          // RNS AutoInterface peering on the AP
  char     autoGroupId[33] = "";        // "" = RNS default "reticulum"
  uint8_t  powerProfile = 0;            // 0 performance, 1 balanced, 2 battery (Power.h)
};

struct AdminSettings {
  char password[33] = ADMIN_PASSWORD_DEFAULT;
};

class Settings {
public:
  void load();                            // call once, before anything else

  const RadioSettings& radio() const { return _radio; }
  const WifiSettings&  wifi()  const { return _wifi;  }
  const AdminSettings& admin() const { return _admin; }
  const TransportSettings& transport() const { return _transport; }

  bool saveRadio(const RadioSettings& r);
  bool saveWifi(const WifiSettings& w);
  bool saveAdminPassword(const char* password);
  bool saveTransport(const TransportSettings& t);
  void factoryReset();                    // wipes the namespace, restores defaults

  // Valid LoRa bandwidths shared by SX126x and SX127x, in kHz.
  static bool validBandwidth(float khz);
  static const char* securityName(ApSecurity s);
  static bool securityFromName(const char* name, ApSecurity& out);

private:
  Preferences   _prefs;
  RadioSettings _radio;
  WifiSettings  _wifi;
  AdminSettings _admin;
  TransportSettings _transport;
};

extern Settings settings;
