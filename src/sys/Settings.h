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
#include "StoreHome.h"

enum class ApSecurity : uint8_t { Open = 0, WPA2 = 1, WPA2_WPA3 = 2, WPA3 = 3 };

struct RadioSettings {
  // Where this node is being operated. It comes first because it decides what
  // the rest may be: 868.1 MHz is a legal channel in Europe and an illegal one
  // in the US, so the band has to be chosen before the channel inside it.
  // Empty means "not chosen yet" and is migrated from the frequency at load.
  char     region[10] = "";
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
  uint8_t  dutyCyclePct   = RF_DUTY_CYCLE_PCT;   // hourly transmit budget, 0 = unlimited
  bool     gpsEnabled     = true;                // boards with a receiver; ignored elsewhere
  bool     gpsSharePosition = false;             // publish coordinates on the public status API
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
//
// There are three of them because the node has three kinds of neighbour and
// they do not want the same policy. A mode is not a preference: RNS refuses
// to broadcast announces onto an access_point interface at all, so a mode
// chosen for one kind of neighbour silences every other kind that shares it.
// wifiMode and autoMode used to be one field, which meant that picking
// "phones come and go" for Sideband also stopped this node exchanging
// announces with the other nodes on the LAN — a mesh of them saw nothing of
// each other and nobody was told why.
struct TransportSettings {
  bool     enabled  = true;
  uint8_t  loraMode = 1;                // full: the LoRa channel is the mesh
  uint8_t  wifiMode = 1;                // clients on :4242 (Sideband, rnsd)
  uint8_t  autoMode = 1;                // AutoInterface peers: other nodes on the LAN
  // rnsd's announce_cap / announce_rate_target / announce_rate_grace /
  // announce_rate_penalty, applied to every interface.
  uint8_t  announceCap        = 2;      // % of interface bandwidth for announces
  uint16_t announceRateTarget = 0;      // s between announces from one destination (0 = off)
  uint8_t  announceRateGrace  = 0;      // violations tolerated before blocking
  uint16_t announceRatePenalty = 0;     // s added to the block
  bool     autoEnabled = true;          // RNS AutoInterface peering on the Wi-Fi links
  char     autoGroupId[33] = "";        // "" = RNS default "reticulum"
  uint8_t  powerProfile = 0;            // 0 performance, 1 balanced, 2 battery (Power.h)
  bool     sdStore = true;              // the Reticulum store's home is the SD card
  // A move of the store that has been asked for and not made yet. It is
  // carried out at the next boot, before anything opens the store, so it has
  // to survive the restart in between — which is what makes it a setting
  // rather than a flag in RAM. StoreHome owns both of these: nothing else may
  // write them, because changing where the store lives without copying it is
  // how a node comes up with an empty path table.
  StoreHome::Move pendingMove = StoreHome::Move::None;
};

struct AdminSettings {
  char password[33] = ADMIN_PASSWORD_DEFAULT;
};

// Which local links run (LocalLink.h). A link the board lacks, or that this
// build has no driver for, is reported as such and its switch is refused by
// the API rather than saved and silently ignored. Wi-Fi off is honoured on
// every board: the node then answers only on USB or PPP where it has them,
// and on the maintenance console everywhere (WIFI ON turns it back on).
struct LinkSettings {
  bool wifiEnabled = true;
  bool usbEnabled  = true;              // USB networking, where the board and build carry it
  bool pppEnabled  = false;             // PPP over the bridge UART, likewise
  // The serial port's speed while PPP is on — console and log included,
  // since they share the port (PppUart.h). Refused unless the board's
  // registry entry lists it and it is no faster than the board has been
  // tried at; the default is the console's speed, so nothing changes until
  // somebody asks.
  uint32_t pppBaud = PPP_BAUD_DEFAULT;
};

// Maintenance surfaces. The bootloader API is on by default because the
// flashing workflow depends on it, and it is guarded by the admin password
// and by the link the request arrives over; a deployed relay can switch it
// off here and be flashed by hand only.
// What the glass does, on boards that have one worth configuring. Its own
// section because it will grow — brightness, sleep timing — and because a
// UI behaviour is neither radio nor maintenance.
struct DisplaySettings {
  // Whether a tap wakes a blanked panel. On by the phone convention; off for
  // a pocketed device whose every accidental touch would light the glass.
  bool touchWake = true;
  // Backlight, percent. The panel maps it through PWM; zero is legal and
  // means "as dark as the glass can show".
  uint8_t brightness = 80;
};

struct MaintenanceSettings {
  bool bootloaderApi     = true;        // POST /api/system/bootloader answers at all
  bool bootloaderFromLan = false;       // ...also from the station (upstream LAN) link
  bool consoleEnabled    = true;        // the serial maintenance console reads commands
  bool consoleTcp        = true;        // ...and answers on CONSOLE_TCP_PORT too, after AUTH
  // The web portal. Off means the routes are never registered and nothing
  // listens on port 80, which is the largest single thing a small board can
  // decline: 28624 B of byte-addressable RAM on a Heltec Wireless Stick with
  // Wi-Fi on, against 272 B for a console listener that does the same job for
  // an operator (ConsoleServer.h). Restart-applied, like Wi-Fi itself.
  bool webUi             = true;
  // mDNS answers <hostname>.local and advertises the RNS port. A convenience
  // rather than something a node depends on, and one of the more expensive
  // ones — which is why the default comes from the board class (Config.h).
  bool mdns              = MDNS_ENABLED_DEFAULT;
  // Whether a message from a listed identity may run a console command
  // (RnsAdmin.h). Off, and an empty list is off however this reads: the
  // delivery address is reachable by anyone who can route to it, so this is
  // the one switch in here that hands the node to somebody who is not
  // holding it.
  bool rnsAdmin          = false;
  // Up to four source hashes, hex, comma separated. Four thirty-two-digit
  // hashes and three commas is 131 characters.
  char rnsAdmins[140]    = "";
  // Whether the node answers a client's ping, echo and signal-report commands
  // (LxmfCommands.h). On, unlike the switch above, and the difference is the
  // whole distinction between the two: these change nothing and disclose
  // nothing, and the person who most needs a signal report is at the edge of
  // coverage where no list would have them. What they cost is airtime, which
  // is why there is a switch at all — an operator on a crowded channel can
  // decline to spend it.
  bool lxmfCommands      = true;
};

class Settings {
public:
  void load();                            // call once, before anything else

  const RadioSettings& radio() const { return _radio; }
  const WifiSettings&  wifi()  const { return _wifi;  }
  const AdminSettings& admin() const { return _admin; }
  const TransportSettings& transport() const { return _transport; }
  const LinkSettings& links() const { return _links; }
  const MaintenanceSettings& maintenance() const { return _maintenance; }
  const DisplaySettings& display() const { return _display; }

  bool saveRadio(const RadioSettings& r);
  bool saveLinks(const LinkSettings& l);
  bool saveMaintenance(const MaintenanceSettings& m);
  bool saveWifi(const WifiSettings& w);
  bool saveAdminPassword(const char* password);
  bool saveTransport(const TransportSettings& t);
  bool saveDisplay(const DisplaySettings& d);
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
  LinkSettings      _links;
  MaintenanceSettings _maintenance;
  DisplaySettings     _display;
};

extern Settings settings;
