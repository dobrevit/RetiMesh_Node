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
//  Maintenance.cpp — see Maintenance.h and MaintenanceProtocol.h.
// ============================================================================
#include "Maintenance.h"
#include "MaintenanceProtocol.h"

#include <esp_heap_caps.h>
#include "Config.h"
#include "Settings.h"
#include "Bootloader.h"
#include "LocalLink.h"
#include "Diag.h"
#include "RnsTransport.h"

namespace Maintenance {

static Stream*       sIo = nullptr;
static LineAssembler sLines;
static char          sOut[160];

// Bytes read per poll. The loop task runs every 200 ms and a typed command is
// a dozen bytes; a host script pasting a line is under a hundred. Anything
// arriving faster than this is not a command, and reading it all at once
// would let a runaway port hold the loop task.
static const size_t kBudget = 128;

static void send(size_t n) {
  if (!sIo) return;
  sIo->write((const uint8_t*)sOut, n < sizeof(sOut) ? n : sizeof(sOut) - 1);
  sIo->write('\n');
}
static void data(const char* cmd, const char* kv) { send(formatData(sOut, sizeof(sOut), cmd, kv)); }
static void ok(const char* cmd, const char* kv = nullptr) { send(formatOk(sOut, sizeof(sOut), cmd, kv)); }
static void err(const char* cmd, int code, const char* text) { send(formatErr(sOut, sizeof(sOut), cmd, code, text)); }

static void dataf(const char* cmd, const char* fmt, ...) {
  char kv[128];
  va_list ap; va_start(ap, fmt);
  vsnprintf(kv, sizeof(kv), fmt, ap);
  va_end(ap);
  data(cmd, kv);
}

// --- handlers ---------------------------------------------------------------
static void doHelp() {
  size_t n = 0;
  const CmdInfo* all = commands(n);
  for (size_t i = 0; i < n; i++)
    dataf("HELP", "cmd=%s%s%s help=\"%s\"", all[i].name, all[i].args[0] ? " args=" : "", all[i].args, all[i].help);
  ok("HELP");
}

static void doVersion() {
  dataf("VERSION", "firmware=\"%s\" version=%s board=\"%s\" idf=%s assets=%s",
        FW_NAME, FW_VERSION, BOARD_NAME, esp_get_idf_version(), ASSET_STAMP);
  ok("VERSION");
}

static void doStatus() {
  const Diag::Boot& b = Diag::boot();
  dataf("STATUS", "uptime_s=%lu boot_count=%lu reset=\"%s\"", (unsigned long)(millis() / 1000),
        (unsigned long)b.count, b.reasonName);
  const Diag::Heap h = Diag::heap();
  dataf("STATUS", "heap_free=%lu heap_min=%lu largest_block=%lu psram_free=%lu",
        (unsigned long)h.freeInternal, (unsigned long)h.minFreeInternal,
        (unsigned long)h.largestBlock, (unsigned long)h.freePsram);
  dataf("STATUS", "radio=%s model=%s rx=%lu tx=%lu", g_stats.radioOnline ? "online" : "offline",
        g_stats.radioModel, (unsigned long)g_stats.loraRxPackets, (unsigned long)g_stats.loraTxPackets);
  dataf("STATUS", "transport=%s tcp_clients=%lu restart_pending=%s",
        g_stats.transportOnline ? "online" : "offline", (unsigned long)g_stats.tcpClients,
        Bootloader::pending() ? "true" : "false");
  ok("STATUS");
}

static void doUsbStatus() {
  #if BOARD_USB_NATIVE
    dataf("USB_STATUS", "native=true personality=%s ncm=%s cdc_acm=%s",
          BOARD_USB_CDC_OTG ? "otg_composite" : "usb_serial_jtag",
          BOARD_USB_NCM ? "hardware" : "no", BOARD_USB_CDC_OTG ? "yes" : "via_serial_jtag");
  #else
    dataf("USB_STATUS", "native=false bridge=%s uart_network=%s auto_reset=%s",
          BOARD_USB_BRIDGE, BOARD_UART_NETWORK ? "hardware" : "no", BOARD_BRIDGE_AUTO_RESET ? "yes" : "no");
  #endif
  const Bootloader::Plan p = Bootloader::plan();
  char methods[96] = "";
  for (size_t i = 0; i < p.count; i++) {
    if (i) strlcat(methods, ",", sizeof(methods));
    strlcat(methods, Bootloader::methodName(p.methods[i]), sizeof(methods));
  }
  dataf("USB_STATUS", "bootloader_methods=%s software_entry=%s", methods,
        Bootloader::canEnterAutomatically() ? "yes" : "no");
  ok("USB_STATUS");
}

static void doNetworkStatus() {
  LocalLink::Snapshot snaps[6];
  const size_t n = LocalLink::snapshots(snaps, 6);
  for (size_t i = 0; i < n; i++) {
    const LocalLink::Snapshot& s = snaps[i];
    char clients[24] = "";
    if (s.clientKnown) snprintf(clients, sizeof(clients), " clients=%u", (unsigned)s.clients);
    char counters[80] = "";
    if (s.counters.known)
      snprintf(counters, sizeof(counters), " rx_bytes=%lu tx_bytes=%lu errors=%lu",
               (unsigned long)s.counters.rxBytes, (unsigned long)s.counters.txBytes,
               (unsigned long)s.counters.errors);
    dataf("NETWORK_STATUS", "link=%s type=%s phase=%s ip=%s addressing=%s uptime_s=%lu%s%s",
          s.name, LocalLink::typeName(s.type), LocalLink::phaseName(s.phase),
          s.ip[0] ? s.ip : "-", LocalLink::addressingName(s.addressing),
          (unsigned long)s.uptimeS, clients, counters);
  }
  ok("NETWORK_STATUS");
}

static void doLinks() {
  for (size_t i = 0; i < LocalLink::count(); i++) {
    const LocalLink::Link* l = LocalLink::at(i);
    const char* why = LocalLink::unavailableReason(*l);
    dataf("LINKS", "link=%s type=%s hardware=%s firmware=%s enabled=%s%s%s%s",
          l->name(), LocalLink::typeName(l->type()), l->hardware() ? "yes" : "no",
          l->firmware() ? "yes" : "no", l->enabled() ? "yes" : "no",
          why[0] ? " reason=\"" : "", why, why[0] ? "\"" : "");
  }
  ok("LINKS");
}

static void doWifi(const Request& r) {
  LinkSettings ls = settings.links();
  const bool on = strcmp(r.args[0], "ON") == 0;
  if (ls.wifiEnabled == on) { ok("WIFI", on ? "wifi=on unchanged=true" : "wifi=off unchanged=true"); return; }
  ls.wifiEnabled = on;
  if (!settings.saveLinks(ls)) { err("WIFI", 500, "NVS write failed"); return; }
  ok("WIFI", on ? "wifi=on restart=true" : "wifi=off restart=true");
  Bootloader::reboot(500, Bootloader::Source::Console);
}

static void doRestart(const Request& r, Bootloader::Target target) {
  const char* name = cmdName(r.cmd);
  if (!r.confirmed) {
    char text[64];
    snprintf(text, sizeof(text), "add CONFIRM: %s CONFIRM", name);
    err(name, 400, text);
    return;
  }
  const char* why = nullptr;
  // The reply has to leave before the port goes away. 300 ms is generous for
  // a few hundred bytes on any of these ports and short enough that a script
  // waiting for the downloader is not kept guessing.
  if (!Bootloader::request(target, Bootloader::Source::Console, 300, &why)) {
    err(name, target == Bootloader::Target::Bootloader && !Bootloader::canEnterAutomatically() ? 501 : 409, why);
    return;
  }
  char kv[96];
  snprintf(kv, sizeof(kv), "target=%s method=%s delay_ms=300", Bootloader::targetName(target),
           target == Bootloader::Target::Bootloader ? Bootloader::methodName(Bootloader::Method::SoftwareApi) : "esp_restart");
  ok(name, kv);
}

static void dispatch(const char* line) {
  Request r;
  const ParseError e = parse(line, r);
  if (e != ParseError::None) {
    err(r.word[0] ? r.word : "?", errorCode(e), errorText(e));
    return;
  }
  switch (r.cmd) {
    case Cmd::Help:          doHelp(); break;
    case Cmd::Status:        doStatus(); break;
    case Cmd::Version:       doVersion(); break;
    case Cmd::UsbStatus:     doUsbStatus(); break;
    case Cmd::NetworkStatus: doNetworkStatus(); break;
    case Cmd::Links:         doLinks(); break;
    case Cmd::Wifi:          doWifi(r); break;
    case Cmd::Reset:         doRestart(r, Bootloader::Target::App); break;
    case Cmd::Bootloader:    doRestart(r, Bootloader::Target::Bootloader); break;
    case Cmd::Unknown:       err(r.word, 404, errorText(ParseError::Unknown)); break;
  }
}

// --- entry points -------------------------------------------------------------
void begin(Stream& io) {
  sIo = &io;
  sLines.reset();
  dataf("HELLO", "firmware=\"%s\" version=%s board=\"%s\" protocol=1", FW_NAME, FW_VERSION, BOARD_NAME);
}

void poll() {
  if (!sIo || !settings.maintenance().consoleEnabled) return;
  size_t budget = kBudget;
  while (budget-- && sIo->available() > 0) {
    const int c = sIo->read();
    if (c < 0) break;
    bool overflowed = false;
    if (sLines.feed((char)c, overflowed)) dispatch(sLines.line());
    else if (overflowed) err("?", errorCode(ParseError::TooLong), errorText(ParseError::TooLong));
  }
}

} // namespace Maintenance
