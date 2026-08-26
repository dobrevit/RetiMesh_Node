// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  RnsTransport.h — microReticulum Transport integration (spike)
// ============================================================================
#pragma once
#include <Arduino.h>

namespace RnsTransport {
  bool begin();          // creates the Reticulum instance with transport enabled
  void loop();           // must be called from one task only (the RNS task)
  size_t pathCount();
}
