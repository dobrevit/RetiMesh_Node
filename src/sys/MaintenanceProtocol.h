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
//  MaintenanceProtocol.h — the maintenance console's wire format
//
//  A small line protocol for the serial port a host has anyway: the S3's USB
//  CDC today, the CDC-ACM function of the composite device once that exists,
//  the CP2102 UART on the Heltec boards. One request per line; every reply
//  line begins with "RM " so a host can pick replies out of the log lines
//  that share the port without a second channel:
//
//      > VERSION
//      RM VERSION firmware="RetiMesh Node" version=v0.2.0 board="LilyGO T3-S3" idf=v4.4.7
//      RM OK VERSION
//
//      > BOOTLOADER
//      RM ERR BOOTLOADER 400 add CONFIRM: BOOTLOADER CONFIRM
//
//      > BOOTLOADER CONFIRM
//      RM OK BOOTLOADER method=software_api delay_ms=600
//
//  Replies: zero or more "RM <CMD> key=value ..." data lines, then exactly
//  one "RM OK <CMD> [key=value ...]" or "RM ERR <CMD> <code> <text>". Codes
//  borrow HTTP's: 400 bad request, 404 unknown command, 409 cannot right now,
//  501 this board cannot. Values with spaces are double-quoted.
//
//  Commands are matched without regard to case. Anything the parser refuses
//  is answered rather than ignored, because a host script waiting on a line
//  that never comes is the worst outcome a serial protocol has.
//
//  This header is pure: the parser, the line assembler and the reply
//  formatter. What the commands do lives in Maintenance.cpp.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

namespace Maintenance {

// A macro rather than a static array: the formatters below are inline and
// odr-use it, and a static in a header gives every translation unit its own
// object — a different definition of the same inline function per file. A
// literal also concatenates into the format strings.
#define REPLY_PREFIX "RM "
// Announced by the HELLO banner. 2 added the line count on every OK line.
#define MAINT_PROTOCOL_VERSION 2
constexpr size_t MAX_LINE = 96;           // request line, bytes, excluding the newline
constexpr size_t MAX_ARGS = 3;
// Long enough for the longest settings key a SET can name
// (transport.announce_rate_penalty, 31), which is what decides this rather
// than any command word.
constexpr size_t MAX_ARG  = 40;

enum class Cmd : uint8_t {
  Help = 0, Status, Version, Reset, Bootloader, UsbStatus, NetworkStatus, Links, Wifi, Ppp,
  Get, Set, Auth, Messages, I2c,
  Unknown,
};

struct CmdInfo { Cmd cmd; const char* name; const char* args; const char* help; };

// The vocabulary, in the order HELP lists it.
inline const CmdInfo* commands(size_t& count) {
  static const CmdInfo kCommands[] = {
    { Cmd::Help,          "HELP",           "",        "list commands" },
    { Cmd::Status,        "STATUS",         "",        "uptime, heap, radio, transport" },
    { Cmd::Version,       "VERSION",        "",        "firmware, board, SDK" },
    { Cmd::UsbStatus,     "USB_STATUS",     "",        "how the host is attached" },
    { Cmd::NetworkStatus, "NETWORK_STATUS", "",        "every local link: phase, address, counters" },
    { Cmd::Links,         "LINKS",          "",        "which links this board offers and which are enabled" },
    { Cmd::Messages,      "MESSAGES",       "[n]",     "the last LXMF messages, newest first (default 10, up to 50)" },
    { Cmd::I2c,           "I2C",            "[0xNN]",  "scan every I2C bus now; with an address, dump that device's first 16 registers" },
    { Cmd::Wifi,          "WIFI",           "ON|OFF",  "enable or disable Wi-Fi (saves, restarts)" },
    { Cmd::Ppp,           "PPP",            "ON|OFF",  "enable or disable PPP on this port (saves, applies live)" },
    { Cmd::Get,           "GET",            "[key]",   "read settings: all, one section (radio), or one key (radio.sf)" },
    { Cmd::Set,           "SET",            "key value", "change one setting; the value is taken as typed" },
    { Cmd::Auth,          "AUTH",           "password", "prove yourself; a session that arrived over the network answers nothing else until you do" },
    { Cmd::Reset,         "RESET",          "CONFIRM", "restart into the application" },
    { Cmd::Bootloader,    "BOOTLOADER",     "CONFIRM", "restart into the ROM downloader for flashing" },
  };
  count = sizeof(kCommands) / sizeof(kCommands[0]);
  return kCommands;
}

inline const char* cmdName(Cmd c) {
  size_t n = 0;
  const CmdInfo* all = commands(n);
  for (size_t i = 0; i < n; i++) if (all[i].cmd == c) return all[i].name;
  return "?";
}

enum class ParseError : uint8_t { None = 0, Empty, TooLong, Unknown, BadArgument, Unauthorised };

struct Request {
  Cmd    cmd = Cmd::Unknown;
  char   word[MAX_ARG] = "";             // the command as typed, uppercased
  char   args[MAX_ARGS][MAX_ARG] = {};
  size_t argc = 0;
  bool   confirmed = false;              // "CONFIRM" appeared among the arguments
  // SET's value, verbatim: everything after the key, with the case it was
  // typed in and any spaces it contained. The tokens above are uppercased,
  // which is right for a command and wrong for a password, an SSID or a
  // callsign. It points into the line the caller parsed, which outlives the
  // request; empty for every other command.
  const char* rawValue = nullptr;
  size_t      rawValueLen = 0;
  bool   hasArg(const char* a) const {
    for (size_t i = 0; i < argc; i++) if (strcmp(args[i], a) == 0) return true;
    return false;
  }
};

// Uppercase ASCII copy, bounded. Anything outside printable ASCII makes the
// token unusable — the port may carry line noise, and noise is not a command.
inline bool copyToken(const char* in, size_t len, char* out, size_t outLen) {
  if (len + 1 > outLen) return false;
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)in[i];
    if (c < 0x21 || c > 0x7E) return false;
    out[i] = (char)toupper(c);
  }
  out[len] = '\0';
  return true;
}

// Which commands take the rest of the line as typed rather than tokenised,
// and after how many arguments: SET's value begins after its key, AUTH's
// password is the whole tail. Both need their case and their spaces — a
// password uppercased is a password that will not authenticate, and an SSID
// with a space in it has to survive the parser. SIZE_MAX means "tokenise
// everything", which is every other command.
inline size_t rawTailAfter(const char* word) {
  if (strcmp(word, "SET") == 0)  return 1;
  if (strcmp(word, "AUTH") == 0) return 0;
  return (size_t)-1;
}

inline void captureRawTail(const char* p, Request& out) {
  const char* v = p;
  while (*v == ' ' || *v == '\t') v++;
  const char* end = v + strlen(v);
  while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
  out.rawValue = v;
  out.rawValueLen = (size_t)(end - v);
}

// One line -> one request. Leading and trailing whitespace is ignored; a
// carriage return counts as whitespace so a terminal that sends CRLF works.
inline ParseError parse(const char* line, Request& out) {
  out = Request();
  if (!line) return ParseError::Empty;
  const size_t total = strlen(line);
  if (total > MAX_LINE) return ParseError::TooLong;

  const char* p = line;
  size_t nTokens = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    const size_t len = (size_t)(p - start);
    if (nTokens == 0) {
      if (!copyToken(start, len, out.word, sizeof(out.word))) return ParseError::BadArgument;
      nTokens++;
      if (rawTailAfter(out.word) == 0) { captureRawTail(p, out); break; }
      continue;
    }
    if (out.argc >= MAX_ARGS) return ParseError::BadArgument;
    if (!copyToken(start, len, out.args[out.argc], MAX_ARG)) return ParseError::BadArgument;
    out.argc++;
    nTokens++;
    if (out.argc == rawTailAfter(out.word)) { captureRawTail(p, out); break; }
  }
  if (nTokens == 0) return ParseError::Empty;

  size_t n = 0;
  const CmdInfo* all = commands(n);
  for (size_t i = 0; i < n; i++)
    if (strcmp(out.word, all[i].name) == 0) { out.cmd = all[i].cmd; break; }
  if (out.cmd == Cmd::Unknown) return ParseError::Unknown;
  out.confirmed = out.hasArg("CONFIRM");

  // Argument shapes, checked here so a handler never sees a request it has
  // to refuse for its form.
  switch (out.cmd) {
    case Cmd::Auth:
      // The password is the tail, so argc says nothing about it.
      if (!out.rawValue || out.rawValueLen == 0) return ParseError::BadArgument;
      break;
    case Cmd::Wifi: case Cmd::Ppp:
      if (out.argc != 1 || (strcmp(out.args[0], "ON") != 0 && strcmp(out.args[0], "OFF") != 0))
        return ParseError::BadArgument;
      break;
    case Cmd::Get:
      // No argument reads everything; one names a section or a single key.
      if (out.argc > 1) return ParseError::BadArgument;
      break;
    case Cmd::Messages:
      // No argument shows a screenful; one says how many. What counts as a
      // number is the handler's business, not the parser's.
      if (out.argc > 1) return ParseError::BadArgument;
      break;
    case Cmd::I2c:
      // No argument scans every bus; one names a device to look inside. What
      // counts as an address is the handler's business, as above.
      if (out.argc > 1) return ParseError::BadArgument;
      break;
    case Cmd::Set:
      // A key and a value, and the value may not be empty — "SET x" with
      // nothing after it is a typo, not a request to blank a setting.
      if (out.argc != 1 || out.rawValueLen == 0) return ParseError::BadArgument;
      break;
    case Cmd::Reset: case Cmd::Bootloader:
      // CONFIRM is optional at parse time; a request without it gets a 400
      // that tells the operator what to type. Anything else is a typo.
      if (out.argc > 1 || (out.argc == 1 && !out.confirmed)) return ParseError::BadArgument;
      break;
    default:
      if (out.argc != 0) return ParseError::BadArgument;
      break;
  }
  return ParseError::None;
}

// Bytes -> lines, with a hard cap. An overlong line is not truncated into a
// shorter, plausible one; it is dropped whole, reported once, and the
// assembler resynchronises at the next newline.
// A partial line that stalls is dropped. Anything that opens the port and
// writes a few bytes without a newline — ModemManager probing a new port
// with AT commands is the usual culprit — would otherwise leave those bytes
// waiting, and the next real command would be glued onto them and refused.
// The clock runs only while the port is silent: bytes that are waiting to be
// read are not a stall, however long the loop took to get to them. Ten
// seconds is a typist's pause, not a prober's.
constexpr uint32_t LINE_IDLE_MS = 10000;

class LineAssembler {
public:
  // Returns true when a complete line is available in line(). `overflowed`
  // is set (once, on the byte that ended the overlong line) so the caller can
  // answer with a TooLong error. `nowMs` stamps the byte for idle().
  bool feed(char c, bool& overflowed, uint32_t nowMs) {
    overflowed = false;
    _lastByteMs = nowMs;
    if (c == '\n' || c == '\r') {
      if (_dropping) { _dropping = false; _len = 0; overflowed = true; return false; }
      if (_len == 0) return false;        // bare newline, or the LF of a CRLF
      _buf[_len] = '\0';
      return true;
    }
    if (_dropping) return false;
    if (_len >= MAX_LINE) { _dropping = true; _len = 0; return false; }
    _buf[_len++] = c;
    return false;
  }
  const char* line() { _len = 0; return _buf; }
  void reset() { _len = 0; _dropping = false; }
  bool pending() const { return _len > 0 || _dropping; }
  // Called when the port has nothing to read: drops a partial line the port
  // has been silent on for LINE_IDLE_MS. True when it dropped one. An
  // overlong line being dropped stays in that state: its remainder, if it
  // ever arrives, is still swallowed to the newline and reported, not
  // assembled into a fresh line that might parse.
  bool idle(uint32_t nowMs) {
    if (_len == 0 || nowMs - _lastByteMs <= LINE_IDLE_MS) return false;
    _len = 0;
    return true;
  }

private:
  char     _buf[MAX_LINE + 1] = {0};
  size_t   _len = 0;
  bool     _dropping = false;
  uint32_t _lastByteMs = 0;
};

// Reply formatting into a caller-supplied buffer. Every function returns the
// length written (without the newline) and never overruns.
inline size_t formatData(char* out, size_t len, const char* cmd, const char* kv) {
  return (size_t)snprintf(out, len, REPLY_PREFIX "%s %s", cmd, kv);
}
// The OK line carries the number of data lines that preceded it, so a host
// can tell a complete reply from one with a line missing rather than guess:
// a reply that is one line short parses just as well as a whole one. A reply
// with no data lines says lines=0. The pair comes first, before any
// command-specific ones, so a reader finds it in the same place every time.
inline size_t formatOk(char* out, size_t len, const char* cmd, unsigned lines, const char* kv = nullptr) {
  if (kv && *kv) return (size_t)snprintf(out, len, REPLY_PREFIX "OK %s lines=%u %s", cmd, lines, kv);
  return (size_t)snprintf(out, len, REPLY_PREFIX "OK %s lines=%u", cmd, lines);
}
inline size_t formatErr(char* out, size_t len, const char* cmd, int code, const char* text) {
  return (size_t)snprintf(out, len, REPLY_PREFIX "ERR %s %d %s", cmd, code, text);
}

inline int errorCode(ParseError e) {
  switch (e) {
    case ParseError::None:        return 0;
    case ParseError::Empty:       return 400;
    case ParseError::TooLong:     return 400;
    case ParseError::Unknown:     return 404;
    case ParseError::BadArgument: return 400;
    case ParseError::Unauthorised: return 401;
  }
  return 400;
}

inline const char* errorText(ParseError e) {
  switch (e) {
    case ParseError::None:        return "";
    case ParseError::Empty:       return "empty line";
    case ParseError::TooLong:     return "line too long";
    case ParseError::Unknown:     return "unknown command, try HELP";
    case ParseError::BadArgument: return "bad argument, try HELP";
    case ParseError::Unauthorised: return "authenticate first: AUTH <password>";
  }
  return "";
}

} // namespace Maintenance
