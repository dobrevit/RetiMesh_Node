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
//  Power.h — power profiles and battery gauge
//
//  Profiles (settings, default "performance" = today's behaviour):
//    performance  240 MHz, Wi-Fi always on, display sleeps after 60 s
//    balanced     160 MHz, Wi-Fi min modem sleep (wake every DTIM)
//    battery       80 MHz, Wi-Fi max modem sleep (wake every
//                 wifi.sta_listen_interval beacons), display sleeps after 20 s
//  LoRa and Transport timing are unaffected by the CPU clock at these
//  rates; the radio task waits on interrupts either way.
//
//  Battery: boards with a power-management chip (HAS_PMU) ask it — it knows
//  whether a cell is connected and whether it is charging. Otherwise the
//  T3-S3 exposes the cell through a 100k/100k divider on
//  GPIO 1. Voltage below BATTERY_MIN_V means "no battery" (USB-only bench).
// ============================================================================
#pragma once

#include <Arduino.h>
#include "Config.h"

namespace Power {

enum class Profile : uint8_t { Performance = 0, Balanced = 1, Battery = 2 };

// What this node *is*, which is a different question from how hard it is
// trying to save power. The profile is a dial the operator turns and can turn
// back; the role is a statement about the installation, and it answers things
// a dial cannot — whether the receiver may stop looking for a position that
// has not changed since the node was bolted to its mast, and, in time, what a
// node left on a solar panel at the end of a track is allowed to do about
// everything else.
//
// The values are persisted (transport.node_role), so they are fixed for ever
// and later roles are added above rather than inserted between:
//
//   Unset      nobody has said. Every rule that reads this must behave exactly
//              as the firmware did before the rule existed, which is what makes
//              upgrading a fleet that never asked for any of this a no-change.
//              It is also what an unrecognised value means — a role written by
//              a later build and read back by an older one — so the same branch
//              covers both. Reading, that is: SettingsRules rejects a stored
//              role above the highest this build knows, exactly as it does the
//              power profile, so a node downgraded while carrying a future
//              role behaves safely but refuses every transport settings change
//              until the role is rewritten to one this build recognises.
//   Carried    a handheld: it moves, somebody looks at its screen, and its
//              position is worth having reasonably fresh.
//   Transport  a fixed installation: it does not move, usually nobody is
//              looking at it, and its position is worth having at all mostly
//              to notice that it has been moved.
//
// Deliberately not inferred from the board class. The same firmware on the
// same board is a handheld on one desk and a relay on the next, and a guess
// dressed as a fact is worse than a field left unset.
enum class Role : uint8_t { Unset = 0, Carried = 1, Transport = 2 };

void begin();                       // applies the configured profile, starts sampling
void apply(Profile p);              // live switch
// Re-applies the current profile's modem sleep setting to the Wi-Fi driver —
// esp_wifi_set_ps() directly, because WiFi.setSleep() only reaches it while
// the STA interface is started and an AP-only node never starts one. Before
// the driver exists the direct call is a harmless no-op, so a caller that
// changes WiFi.mode() (bringing an interface up or back) must call this
// again afterwards; WifiManager's STA_START/AP_START hook does exactly that.
void applyWifiSleep();
// Re-applies the configured Wi-Fi TX ceiling (wifi.tx_power, dBm) to the
// driver — esp_wifi_set_max_tx_power, one global ceiling for the AP and the
// station together. Same lifecycle as applyWifiSleep(): it fails harmlessly
// until the driver is started, so the STA_START/AP_START hook calls it again
// the moment an interface comes up, and the settings commit calls it for a
// live change.
void applyWifiTxPower();
Profile profile();

// The persisted role. Nothing has to be applied when it changes — the rules
// that consult it ask on their own schedule — so this stays a read rather than
// becoming a broadcast like the profile. It is read from Settings::nodeRole(),
// the atomic the settings keep in step with the transport struct, because the
// GNSS reader asks it ten times a second on its own task while a commit from
// the web, console or LXMF task replaces that struct wholesale. A stored value
// this build does not recognise is handed on as it stands; every reader is
// required to treat an unknown role as Unset, and roleName() says so.
Role role();
const char* roleName(Role r);
bool roleFromName(const char* name, Role& out);

// The screen has gone dark, or has come back. One call, from the node's only
// screen-state edge (Display::setBlank), and from here it reaches every part
// that has nothing to do while nobody is looking: today the magnetometer and
// the accelerometer, on the two boards that carry them. What each part does
// with it is that part's own decision — both record the request and write it
// from their own poll(), on the main loop — so this is safe from any task and
// costs nothing on a board with neither part fitted.
//
// Deferred because it keeps each part's register writes on one task and the
// drivers idempotent, not because writing from here would corrupt somebody
// else's transaction: TwoWire locks a transaction end to end, and the window
// the bus really leaves open is on the read side, where I2cReg drains the
// receive buffer after the lock has gone (Imu.cpp says which boards that
// exposes). This call itself only stores flags.
//
// The rule for which part follows the screen and which follows the profile
// lives in PeripheralPolicy.h; apply() below re-asks it, so a profile change
// and a screen change are the same broadcast rather than two.
void onScreenBlank(bool dark);

// Whether the screen is dark, as the broadcast above last recorded it. The one
// answer to that question in the firmware: the display task owns the edge and
// several things now need the state, and two modules each keeping their own
// copy of "is anybody looking" is the arrangement where one of them misses an
// edge and stays quiesced under a lit panel. False on a board with no display,
// which is the truthful answer there — nothing is being looked at, but nothing
// is going dark either, so a rule that only fires while dark must not fire.
bool screenDark();

// The node is about to stop: deep sleep, or the charger's ship mode, both
// reached from the power menu with the panel already blanked. Deep sleep holds
// the pins as they stand, so a part still converting when it starts goes on
// converting, off the same cell, behind a menu item named "off" — and
// onScreenBlank() above has only recorded the request. This waits, bounded,
// for the parts to have actually stopped, and flushes what would otherwise be
// lost with the rail. Blocking, up to a fifth of a second; call it from the
// task that is about to sleep the node, after the screen has gone dark.
void prepareForSleep();

const char* profileName(Profile p);

// What is managing the cell on this board, by part number: "AXP192",
// "AXP2101", "BQ25896" or "none".
//
// One accessor rather than each surface deciding, because the answer is not
// Pmu::model() alone: the V4 has no power-management chip at all (HAS_PMU 0)
// and a BQ25896 charger beside a plain divider, so asking the PMU there
// returns "none" and describes the one board on this bench that *can* answer a
// charging question as though it could not. The console and the HTTP API both
// used to ask Pmu directly and both were wrong on that board.
//
// It matters beyond tidiness: which part is fitted decides what the board can
// be *asked*. An AXP192 reports its own discharge current and carries a
// coulomb counter; an AXP2101 reports voltages only; a BQ25896 gives charge
// current but not discharge. A power measurement has to know which of those it
// is standing in front of.
const char* chargerName();
bool profileFromName(const char* name, Profile& out);
// What the Wi-Fi driver is actually doing about modem sleep, read back from
// esp_wifi_get_ps() rather than assumed from the profile: the two can
// disagree — applyWifiSleep() is a no-op until the driver exists (see
// above), so this is the one place STATUS/portal can show whether a profile
// switch actually took. "n/a" with Wi-Fi off: the getter answers even with
// no driver running, so the off state comes from the links settings'
// wifiEnabled() rule (Settings.h), not from the getter.
const char* wifiPsName();
// The TX ceiling the driver actually holds, in dBm — read back from
// esp_wifi_get_max_tx_power rather than echoing the setting, because the
// driver quantizes down to its own quarter-dBm steps (ask for 9, hold 8.5).
// NAN with Wi-Fi off or the driver not up, by wifiPsName()'s rule.
float wifiTxPowerDbm();

struct Battery {
  bool  present;                    // a cell is connected
  bool  charging = false;           // meaningless unless chargeKnown
  // Whether charging is a question this board can answer at all. A divider
  // measures the cell and nothing else: the charger on an ADC-only board does
  // its work in hardware and tells the processor nothing, and there is no
  // status line to read. Reporting "not charging" there is a claim the board
  // cannot support — a plugged-in node insisting it is not charging is worse
  // than one admitting it has no idea, because the first sends someone looking
  // for a fault in a cable that is working.
  bool  chargeKnown = false;
  float volts;
  uint8_t percent;                  // rough LiPo curve; 0 when absent
};
Battery battery();

// Whether the last reading is too old to believe. battery() reports present =
// false in that case, which is the honest answer and the one every caller
// already renders — but it is the same answer as "no cell is fitted", and
// during a bring-up those want telling apart. False on a board with no divider
// to go stale.
bool readingStale();
uint32_t displaySleepMs();          // profile-dependent

// The last eight hours of charge, one percent-point per five minutes,
// oldest first — the discharge sparkline's data. Returns how many exist.
size_t batteryHistory(uint8_t* out, size_t max);

} // namespace Power
