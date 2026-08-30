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
//  QrCode.h — the node as something you can point a camera at
//
//  Three payloads, all built on the device (ricmoo/QRCode, MIT):
//    wifi     WIFI:...  — join the access point without typing the password
//    portal   http://<ip>/ — open the captive portal
//    address  the node's Reticulum destination hash, as text
//
//  The encoder needs a version (size) up front, so buildQr() tries the
//  smallest that fits at ECC level L and reports which one it used. The
//  module buffer belongs to the caller: the OLED renders it directly, the
//  web layer turns it into an SVG of horizontal runs.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <qrcode.h>
#include "Config.h"

namespace Qr {

// Version 8 (49x49) at ECC L holds 152 bytes — far more than any payload
// here — and its buffer is 306 B, which is fine on a task stack.
static const uint8_t MAX_VERSION = 8;
static const size_t  MAX_BUFFER  = 306;      // qrcode_getBufferSize(8)

enum class Payload : uint8_t { Wifi, Portal, Address };

// Fills `text` with the payload for `what`. Returns false if it does not fit.
bool payloadText(Payload what, char* text, size_t cap);

// Encodes `text` into `qr` using `buffer` (>= MAX_BUFFER bytes), picking the
// smallest version that fits. Returns false if even version 8 is too small.
bool encode(const char* text, QRCode& qr, uint8_t* buffer);

// Renders an encoded code as a standalone SVG (quiet zone included).
String toSvg(QRCode& qr, uint16_t pixelsPerModule = 8);

bool parsePayload(const char* name, Payload& out);

} // namespace Qr
