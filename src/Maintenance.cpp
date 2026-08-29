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

#include "Config.h"
#include "Settings.h"
#include "Bootloader.h"
#include "LocalLink.h"
#include "Diag.h"

namespace Maintenance {

static Stream*       sIo = nullptr;
static LineAssembler sLines;
static char          sOut[224];

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
static void ok(const char* cmd, const char* kv = nullptr) { send(formatOk(sOut, sizeof(sOut), cmd, kv)); }
static void err(const char* cmd, int code, const char* text) { send(formatErr(sOut, sizeof(sOut), cmd, code, text)); }

// A data line, formatted straight into the output buffer after its prefix.
// The earlier version formatted into a second buffer on the stack and then
// copied it in here — on the loop task, which also carries the console, the
// diagnostics report and the restart sequence, and whose stack is the one
// nothing in platformio.ini has enlarged.
static void dataf(const char* cmd, const char* fmt, ...) {
  // The line's shape comes from the protocol header — a data line with
  // nothing after the command yet — so this file does not carry a second
  // spelling of it.
  const int head = (int)formatData(sOut, sizeof(sOut), cmd, "");
  if (head < 0 || (size_t)head >= sizeof(sOut)) { send(sizeof(sOut)); return; }
  va_list ap; va_start(ap, fmt);
  const int body = vsnprintf(sOut + head, sizeof(sOut) - (size_t)head, fmt, ap);
  va_end(ap);
  send((size_t)head + (body < 0 ? 0 : (size_t)body));
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
    // The S3 runs its fixed USB-Serial/JTAG personality; the composite device
    // with its own CDC is a build that does not exist yet.
    dataf("USB_STATUS", "native=true personality=usb_serial_jtag ncm=%s cdc_acm=via_serial_jtag",
          BOARD_USB_NCM ? "hardware" : "no");
  #else
    dataf("USB_STATUS", "native=false bridge=%s uart_network=%s auto_reset=%s",
          BOARD_USB_BRIDGE, BOARD_UART_NETWORK ? "hardware" : "no", BOARD_BRIDGE_AUTO_RESET ? "yes" : "no");
  #endif
  char methods[64];
  dataf("USB_STATUS", "bootloader_methods=%s software_entry=%s",
        Bootloader::plan().names(methods, sizeof(methods)),
        Bootloader::canEnterAutomatically() ? "yes" : "no");
  ok("USB_STATUS");
}

static void doNetworkStatus() {
  // One snapshot live at a time, not the whole registry on the stack.
  for (size_t i = 0; i < LocalLink::count(); i++) {
    const LocalLink::Snapshot s = LocalLink::at(i)->snapshot();
    char clients[24] = "";
    if (s.clientKnown) snprintf(clients, sizeof(clients), " clients=%u", (unsigned)s.clients);
    dataf("NETWORK_STATUS", "link=%s type=%s phase=%s ip=%s addressing=%s uptime_s=%lu%s",
          s.name, LocalLink::typeName(s.type), LocalLink::phaseName(s.phase),
          s.ip[0] ? s.ip : "-", LocalLink::addressingName(s.addressing),
          (unsigned long)s.uptimeS, clients);
  }
  ok("NETWORK_STATUS");
}

static void doLinks() {
  for (size_t i = 0; i < LocalLink::count(); i++) {
    const LocalLink::Link* l = LocalLink::at(i);
    const char* why = l->reason();
    dataf("LINKS", "link=%s type=%s hardware=%s firmware=%s enabled=%s%s%s%s",
          l->name(), LocalLink::typeName(l->type()), l->hardware() ? "yes" : "no",
          l->firmware() ? "yes" : "no", l->enabled() ? "yes" : "no",
          why[0] ? " reason=\"" : "", why, why[0] ? "\"" : "");
  }
  ok("LINKS");
}

static void doWifi(const Request& r) {
  // The same rule the HTTP handler applies, in the one place it is written.
  const bool on = strcmp(r.args[0], "ON") == 0;
  LinkSettings want = settings.links();
  want.wifiEnabled = on;
  bool changed[8] = { true };            // only the first field, wifi, is given
  const char* detail = "";
  char kv[96];
  const char* state = on ? "on" : "off";
  switch (LocalLink::applyLinks(want, changed, Bootloader::Source::Console, &detail)) {
    case LocalLink::Apply::Unchanged:        snprintf(kv, sizeof(kv), "wifi=%s unchanged=true", state); ok("WIFI", kv); break;
    case LocalLink::Apply::Saved:            snprintf(kv, sizeof(kv), "wifi=%s restart=false", state); ok("WIFI", kv); break;
    case LocalLink::Apply::SavedRestarting:  snprintf(kv, sizeof(kv), "wifi=%s restart=true", state); ok("WIFI", kv); break;
    case LocalLink::Apply::SavedNextBoot:    snprintf(kv, sizeof(kv), "wifi=%s restart=false note=applies_at_next_boot", state); ok("WIFI", kv); break;
    case LocalLink::Apply::RefusedUnusable:  err("WIFI", 400, detail); break;
    case LocalLink::Apply::RefusedLockedOut: err("WIFI", 400, "refused: with the serial console switched off this would leave no way to reach the node"); break;
    case LocalLink::Apply::RefusedBusy:      err("WIFI", 409, "a restart is already in progress"); break;
    case LocalLink::Apply::NvsFailed:        err("WIFI", 500, "NVS write failed"); break;
  }
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
  // The reply has to leave before the port goes away; the same grace the
  // HTTP path gives its 202, so a host tool can wait on one figure.
  const Bootloader::Refusal r2 = Bootloader::request(target, Bootloader::Source::Console, RESTART_ACK_DELAY_MS, &why);
  if (r2 != Bootloader::Refusal::None) { err(name, Bootloader::httpStatus(r2), why); return; }
  char kv[96];
  snprintf(kv, sizeof(kv), "target=%s method=%s delay_ms=%d", Bootloader::targetName(target),
           target == Bootloader::Target::Bootloader ? Bootloader::methodName(Bootloader::Method::SoftwareApi) : "esp_restart",
           RESTART_ACK_DELAY_MS);
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
    default:                 break;      // Unknown never reaches here: parse() refused it
  }
}

// --- entry points -------------------------------------------------------------
void begin(Stream& io) {
  sIo = &io;
  sLines.reset();
  dataf("HELLO", "firmware=\"%s\" version=%s board=\"%s\" protocol=1", FW_NAME, FW_VERSION, BOARD_NAME);
}

void poll() {
  if (!sIo) return;
  if (!settings.maintenance().consoleEnabled) {
    // Off means off, not deferred. Bytes that arrive while the console is
    // disabled are discarded rather than left in the port's buffer, where an
    // earlier version let them sit until the console was switched back on —
    // at which point a "BOOTLOADER CONFIRM" typed days before was executed.
    size_t budget = kBudget;
    while (budget-- && sIo->available() > 0) sIo->read();
    sLines.reset();
    return;
  }
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
