// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  QrCode.cpp — see QrCode.h
// ============================================================================
#include "QrCode.h"
#include "Settings.h"
#include "WifiManager.h"
#include "RnsAnnounce.h"
#include <WiFi.h>

namespace Qr {

// In a WIFI: payload these characters carry meaning and must be escaped.
static void appendEscaped(String& out, const char* value) {
  for (const char* p = value; *p; p++) {
    if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') out += '\\';
    out += *p;
  }
}

bool parsePayload(const char* name, Payload& out) {
  if (!name || !*name || !strcmp(name, "wifi")) { out = Payload::Wifi;    return true; }
  if (!strcmp(name, "portal"))                  { out = Payload::Portal;  return true; }
  if (!strcmp(name, "address"))                 { out = Payload::Address; return true; }
  return false;
}

bool payloadText(Payload what, char* text, size_t cap) {
  String s;
  switch (what) {
    case Payload::Wifi: {
      const WifiSettings& w = settings.wifi();
      bool open = w.security == ApSecurity::Open;
      s = "WIFI:T:";
      s += open ? "nopass" : "WPA";
      s += ";S:";
      appendEscaped(s, wifiManager.ssid());
      if (!open) { s += ";P:"; appendEscaped(s, w.password); }
      if (w.hidden) s += ";H:true";
      s += ";;";
      break;
    }
    case Payload::Portal: {
      // The address a phone should open: the LAN one when the node joined a
      // network (that is where the operator's browser is), the AP otherwise.
      IPAddress ip = wifiManager.stationConnected() ? WiFi.localIP() : WiFi.softAPIP();
      s = "http://" + ip.toString() + "/";
      break;
    }
    case Payload::Address:
      s = nodeIdentity.destHex();
      break;
  }
  if (s.length() + 1 > cap) return false;
  strlcpy(text, s.c_str(), cap);
  return true;
}

bool encode(const char* text, QRCode& qr, uint8_t* buffer) {
  // qrcode_initText() does not reject input that is too large for the
  // requested version — it returns 0 and emits a code no scanner can read —
  // so the capacity has to be checked here. Byte mode, ECC level L,
  // versions 1..8 (ISO/IEC 18004 table 7).
  static const uint16_t kByteCapacityEccL[MAX_VERSION] = { 17, 32, 53, 78, 106, 134, 154, 192 };
  const size_t len = strlen(text);
  for (uint8_t version = 1; version <= MAX_VERSION; version++) {
    if (len > kByteCapacityEccL[version - 1]) continue;
    if (qrcode_initText(&qr, buffer, version, ECC_LOW, text) == 0) return true;
  }
  return false;
}

String toSvg(QRCode& qr, uint16_t pixelsPerModule) {
  const uint8_t quiet = 4;                       // required by the spec
  const uint16_t span = qr.size + 2 * quiet;
  const uint32_t px   = (uint32_t)span * pixelsPerModule;

  String svg;
  svg.reserve(1024 + (size_t)qr.size * qr.size / 2);
  svg  = F("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"");
  svg += px; svg += F("\" height=\""); svg += px;
  svg += F("\" viewBox=\"0 0 "); svg += span; svg += ' '; svg += span;
  svg += F("\" shape-rendering=\"crispEdges\" role=\"img\">");
  svg += F("<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/><g fill=\"#000000\">");

  // One rect per horizontal run of dark modules instead of one per module:
  // a third of the bytes, and every renderer draws it identically.
  for (uint8_t y = 0; y < qr.size; y++) {
    uint8_t x = 0;
    while (x < qr.size) {
      if (!qrcode_getModule(&qr, x, y)) { x++; continue; }
      uint8_t run = 0;
      while (x + run < qr.size && qrcode_getModule(&qr, x + run, y)) run++;
      svg += F("<rect x=\""); svg += (uint16_t)(x + quiet);
      svg += F("\" y=\"");    svg += (uint16_t)(y + quiet);
      svg += F("\" width=\""); svg += run;
      svg += F("\" height=\"1\"/>");
      x += run;
    }
  }
  svg += F("</g></svg>");
  return svg;
}

} // namespace Qr
