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
#include "SettingsFields.h"

#include "Config.h"
#include "Settings.h"
#include "Bootloader.h"
#include "LocalLink.h"
#include "WifiManager.h"
#include "UsbNcm.h"
#include "PppUart.h"
#include "Diag.h"

namespace Maintenance {

static Stream*       sIo = nullptr;
static LineAssembler sLines;
static char          sOut[224];
static unsigned      sDataLines = 0;   // data lines sent for the command in hand; the OK line reports it

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
static void ok(const char* cmd, const char* kv = nullptr) { send(formatOk(sOut, sizeof(sOut), cmd, sDataLines, kv)); }
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
  sDataLines++;
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
  // What a stack or buffer can actually be placed in (Diag.h): the line above
  // counts IRAM that no byte-addressed allocation can use.
  dataf("STATUS", "dram_free=%lu dram_min=%lu dram_largest_block=%lu",
        (unsigned long)h.freeDram, (unsigned long)h.minFreeDram,
        (unsigned long)h.largestDramBlock);
  dataf("STATUS", "radio=%s model=%s rx=%lu tx=%lu", g_stats.radioOnline ? "online" : "offline",
        g_stats.radioModel, (unsigned long)g_stats.loraRxPackets, (unsigned long)g_stats.loraTxPackets);
  // The armed restart, in the same words /api/status uses: what it is for,
  // who asked, and how long until it fires.
  const Bootloader::Pending p = Bootloader::snapshot();
  char restart[96] = "";
  if (p.armed())
    snprintf(restart, sizeof(restart), " restart_target=%s restart_source=%s restart_in_ms=%lu",
             Bootloader::targetName(p.target), Bootloader::sourceName(p.source),
             (unsigned long)p.dueInMs(millis()));
  dataf("STATUS", "transport=%s tcp_clients=%lu dns=%s restart_pending=%s%s",
        g_stats.transportOnline ? "online" : "offline", (unsigned long)g_stats.tcpClients,
        wifiManager.dnsListening() ? "listening" : "down",
        p.armed() ? "true" : "false", restart);
  ok("STATUS");
}

static void doUsbStatus() {
  #if HAS_USB_NCM
    // The OTG stack owns the peripheral: this port is the composite device's
    // ACM function, and usb0 is its NCM function.
    dataf("USB_STATUS", "native=true personality=usb_otg_composite ncm=driver cdc_acm=composite pid_test_allocation=%s",
          USB_PID_IS_TEST_ALLOCATION ? "yes" : "no");
    dataf("USB_STATUS", "ncm_link=%s host_opened=%s ncm_rx=%lu ncm_tx=%lu ncm_tx_dropped=%lu",
          UsbNcm::linkUp() ? "up" : "down", UsbNcm::hostOpened() ? "yes" : "no", (unsigned long)UsbNcm::rxPackets(),
          (unsigned long)UsbNcm::txPackets(), (unsigned long)UsbNcm::txDropped());
  #elif BOARD_USB_NATIVE
    // The S3's fixed USB-Serial/JTAG personality.
    dataf("USB_STATUS", "native=true personality=usb_serial_jtag ncm=%s cdc_acm=via_serial_jtag",
          BOARD_USB_NCM ? "hardware" : "no");
  #else
    dataf("USB_STATUS", "native=false bridge=%s uart_network=%s auto_reset=%s",
          BOARD_USB_BRIDGE, BOARD_UART_NETWORK ? "driver" : "no", BOARD_BRIDGE_AUTO_RESET ? "yes" : "no");
    #if HAS_PPP
      // Who has the port right now, and how PPP has been going. A reader
      // sees this line only while the console owns the port, which is the
      // point: uart_owner=ppp is what the log line after a session says.
      dataf("USB_STATUS", "uart_owner=%s baud=%lu ppp_sessions=%lu ppp_rx=%lu ppp_tx=%lu ppp_tx_dropped=%lu",
            PppUart::ownerName(PppUart::owner()), (unsigned long)PppUart::baud(), (unsigned long)PppUart::sessions(),
            (unsigned long)PppUart::rxBytes(), (unsigned long)PppUart::txBytes(), (unsigned long)PppUart::txDropped());
    #endif
  #endif
  // Software entry is offered exactly when the plan lists it; the plan is
  // built from the same facts the request path decides on.
  const Bootloader::Plan p = Bootloader::plan();
  char methods[64];
  dataf("USB_STATUS", "bootloader_methods=%s software_entry=%s",
        p.names(methods, sizeof(methods)), p.has(Bootloader::Method::SoftwareApi) ? "yes" : "no");
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
    // The PPP link's speed and the addresses it will ask for ride with it:
    // what a host has to tell its pppd (PppUart.h), read from the port
    // before pppd takes it.
    char baud[80] = "";
    #if HAS_PPP
      if (l->type() == LocalLink::Type::PppUart)
        snprintf(baud, sizeof(baud), " baud=%lu asks=%s peer=%s", (unsigned long)settings.links().pppBaud,
                 PppUart::askedAddress().toString().c_str(), PppUart::askedPeer().toString().c_str());
    #endif
    dataf("LINKS", "link=%s type=%s hardware=%s firmware=%s enabled=%s%s%s%s%s",
          l->name(), LocalLink::typeName(l->type()), l->hardware() ? "yes" : "no",
          l->firmware() ? "yes" : "no", l->enabled() ? "yes" : "no", baud,
          why[0] ? " reason=\"" : "", why, why[0] ? "\"" : "");
  }
  ok("LINKS");
}

// WIFI ON|OFF and PPP ON|OFF: one link's switch, by the key the settings
// table binds it to. The same rule the HTTP handler applies, in the one
// place it is written; the reply names the key it changed.
// WIFI ON and SET links.wifi on are the same operation and now take the same
// path: the mapping from a link key to its switch lives in LocalLink::fields
// and is walked once, in SettingsFields, rather than here as well. The reply
// keeps this command's shape — a key=value on the OK line — because scripts
// read it.
static void doLinkSwitch(const Request& r, const char* key) {
  const char* name = cmdName(r.cmd);
  const bool on = strcmp(r.args[0], "ON") == 0;
  char field[32];
  snprintf(field, sizeof(field), "links.%s", key);
  char detail[192] = "";
  const SettingsFields::Result res = SettingsFields::set(field, on ? "on" : "off", detail, sizeof(detail));
  char kv[128];
  const char* state = on ? "on" : "off";
  switch (res) {
    case SettingsFields::Result::Ok:
      snprintf(kv, sizeof(kv), "%s=%s restart=false", key, state); ok(name, kv); break;
    case SettingsFields::Result::OkRestart:
      snprintf(kv, sizeof(kv), "%s=%s restart=true", key, state); ok(name, kv); break;
    case SettingsFields::Result::OkNextBoot:
      snprintf(kv, sizeof(kv), "%s=%s restart=false note=applies_at_next_boot", key, state); ok(name, kv); break;
    case SettingsFields::Result::Busy:        err(name, 409, "a restart is already in progress"); break;
    case SettingsFields::Result::Unsupported: err(name, 501, detail); break;
    case SettingsFields::Result::NvsFailed:   err(name, 500, "NVS write failed"); break;
    default:                                  err(name, 400, detail[0] ? detail : "refused"); break;
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

// Defined below with the rest of the settings work; declared here because
// the dispatch table comes first.
static void doGet(const Request& r);
static void doSet(const Request& r);

static void dispatch(const char* line) {
  sDataLines = 0;
  // The reply begins on a fresh line. The S3's USB unit drops the last
  // packet it was holding when the host opened the port; when that was the
  // end of a log line, the host's buffer holds an unterminated fragment and
  // the first reply line arrives glued to it — read as log noise, not as a
  // reply. An empty line costs nothing and ends whatever was left hanging.
  if (sIo) sIo->write('\n');
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
    case Cmd::Wifi:          doLinkSwitch(r, "wifi"); break;
    case Cmd::Ppp:           doLinkSwitch(r, "ppp"); break;
    case Cmd::Get:           doGet(r); break;      // defined below, with the other settings work
    case Cmd::Set:           doSet(r); break;
    case Cmd::Reset:         doRestart(r, Bootloader::Target::App); break;
    case Cmd::Bootloader:    doRestart(r, Bootloader::Target::Bootloader); break;
    default:                 break;      // Unknown never reaches here: parse() refused it
  }
}

// --- settings -----------------------------------------------------------------
// GET with nothing reads every setting, GET <section> one section, GET <key>
// one setting. The keys are the web API's names with their section in front,
// so an operator who knows one knows the other (SettingsFields.h).
static void doGet(const Request& r) {
  char line[160];
  if (r.argc == 0) {
    // The section names, not every value. A bare GET over the whole table is
    // forty-odd blocking writes from the loop task — and on a USB CDC whose
    // host has the port open but is not draining it, a write is bounded by
    // nothing. The same file caps reads at kBudget for that reason.
    char seen[8][24] = {};
    size_t sections = 0;
    for (size_t i = 0; i < SettingsFields::count() && sections < 8; i++) {
      const char* key = SettingsFields::keyAt(i);
      const char* dot = strchr(key, '.');
      if (!dot) continue;
      const size_t len = (size_t)(dot - key);
      bool known = false;
      for (size_t s = 0; s < sections; s++) if (!strncmp(seen[s], key, len) && seen[s][len] == '\0') known = true;
      if (known) continue;
      snprintf(seen[sections], sizeof(seen[sections]), "%.*s", (int)len, key);
      sections++;
    }
    for (size_t s = 0; s < sections; s++) dataf("GET", "section=%s", seen[s]);
    ok("GET", "note=\"GET <section> or GET <section>.<key> for values\"");
    return;
  }
  // A key first: "radio.sf" is a key, "radio" a section, and a section that
  // does not exist is a typo worth saying so about rather than an empty list.
  if (SettingsFields::renderKey(r.args[0], line, sizeof(line))) {
    dataf("GET", "%s", line);
    ok("GET");
    return;
  }
  if (!SettingsFields::sectionExists(r.args[0])) {
    err("GET", errorCode(ParseError::BadArgument), "no such setting or section");
    return;
  }
  for (size_t i = 0; i < SettingsFields::count(); i++)
    if (SettingsFields::keyInSection(i, r.args[0]) && SettingsFields::render(i, line, sizeof(line)))
      dataf("GET", "%s", line);
  ok("GET");
}

// SET changes one setting. The value is whatever followed the key on the line,
// as typed; the refusals are the web API's, because both go through the same
// rule (SettingsRules.h).
static void doSet(const Request& r) {
  char value[MAX_LINE + 1];
  size_t n = r.rawValueLen < sizeof(value) ? r.rawValueLen : sizeof(value) - 1;
  const char* src = r.rawValue;
  // Quotes around the value are stripped, which is how a text setting is
  // cleared — SET wifi.sta_ssid "" — and how one keeps spaces at its ends.
  // A bare empty value is refused by the parser as a typo, so without this
  // there would be no way to unset a callsign or a station network from the
  // console, which is the link that has to be able to undo a bad one.
  if (n >= 2 && src[0] == '"' && src[n - 1] == '"') { src++; n -= 2; }
  memcpy(value, src, n);
  value[n] = '\0';

  char detail[192] = "";
  const SettingsFields::Result res = SettingsFields::set(r.args[0], value, detail, sizeof(detail));
  char line[224];
  switch (res) {
    case SettingsFields::Result::Ok:
    case SettingsFields::Result::OkRestart:
    case SettingsFields::Result::OkNextBoot:
      // Read back what was stored rather than echoing what was typed: a value
      // the store rounded, truncated or ignored should show as it now is.
      if (SettingsFields::renderKey(r.args[0], line, sizeof(line))) dataf("SET", "%s", line);
      ok("SET", SettingsFields::resultText(res));
      break;
    case SettingsFields::Result::Unknown:
      err("SET", errorCode(ParseError::BadArgument), "no such setting");
      break;
    case SettingsFields::Result::NvsFailed:
      err("SET", 500, SettingsFields::resultText(res));
      break;
    // The three refusals keep their own codes, as WIFI and PPP have always
    // returned them: 409 is worth retrying, 501 never is, and 400 means the
    // value was wrong. Flattening them onto 400 tells a script to give up on
    // a restart that will be over in a second.
    case SettingsFields::Result::Busy:
      err("SET", 409, detail[0] ? detail : SettingsFields::resultText(res));
      break;
    case SettingsFields::Result::Unsupported:
      err("SET", 501, detail[0] ? detail : SettingsFields::resultText(res));
      break;
    default:
      snprintf(line, sizeof(line), "%s%s%s", SettingsFields::resultText(res),
               detail[0] ? ": " : "", detail);
      err("SET", errorCode(ParseError::BadArgument), line);
      break;
  }
}

// --- entry points -------------------------------------------------------------
void begin(Stream& io) {
  sIo = &io;
  sLines.reset();
  dataf("HELLO", "firmware=\"%s\" version=%s board=\"%s\" protocol=%d", FW_NAME, FW_VERSION, BOARD_NAME,
        MAINT_PROTOCOL_VERSION);
}

void useStream(Stream& io) {
  if (sIo == &io) return;
  // A half-typed line belongs to the stream it was typed on, not to the next
  // one; carrying it across would glue two sources together.
  sLines.reset();
  sIo = &io;
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
  // Nothing to read: the port is silent, which is when a stalled partial
  // line — a prober's leftovers — expires (MaintenanceProtocol.h).
  if (sIo->available() <= 0) {
    if (sLines.idle(millis())) log_d("console: dropped a line the port went silent on");
    return;
  }
  size_t budget = kBudget;
  while (budget-- && sIo->available() > 0) {
    const int c = sIo->read();
    if (c < 0) break;
    bool overflowed = false;
    if (sLines.feed((char)c, overflowed, millis())) dispatch(sLines.line());
    else if (overflowed) err("?", errorCode(ParseError::TooLong), errorText(ParseError::TooLong));
  }
}

} // namespace Maintenance
