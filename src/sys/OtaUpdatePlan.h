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
//  OtaUpdatePlan.h — the rules about taking an update, without the hardware
//
//  Split from OtaUpdate.h for the reason BootloaderPlan.h is split from
//  Bootloader.h: what a node will and will not accept is worth testing, and a
//  rule that can only be reached by booting a board with a card in the slot and
//  a browser pointed at it will not be. Nothing here includes Arduino, the
//  partition API or the card.
// ============================================================================
#pragma once

#include <stdint.h>

namespace Ota {

// Where an update is in its journey. Deliberately one line each: this is what
// the portal shows and what an operator reads back over a link that may be a
// LoRa console session.
enum class Stage : uint8_t {
  Idle,        // nothing in flight
  Receiving,   // bytes are arriving
  Staged,      // the whole bundle is on the card, not yet judged
  Installing,  // being verified and written
  Installed,   // written and switched to; the node restarts into it
  Failed,      // see the message
};

inline const char* describe(Stage s) {
  switch (s) {
    case Stage::Idle:       return "idle";
    case Stage::Receiving:  return "receiving";
    case Stage::Staged:     return "staged";
    case Stage::Installing: return "installing";
    case Stage::Installed:  return "installed";
    case Stage::Failed:     return "failed";
  }
  return "unknown";
}

// Whether an upload may begin, and the sentence to say when it may not. Pure,
// so the portal's button and the endpoint behind it cannot disagree about
// whether the node can take an update at all.
//
// The order is what an operator can do something about. A board with one app
// slot is never going to take an update this way and should say so before a
// missing card is mentioned, because finding a card would not help.
inline const char* uploadRefusal(bool stagingReady, bool haveUpdateSlot, Stage current) {
  if (!haveUpdateSlot)
    return "this board has a single app partition and cannot install its own updates";
  // stagingReady is "somewhere to put the bytes as they arrive": the card on a
  // board with a slot, LittleFS on one without — and LittleFS is mounted at
  // boot and stays mounted, so on a slotless board this refusal cannot occur
  // and the card wording below is accurate everywhere it can be read.
  if (!stagingReady)
    return "an update has to be staged on an SD card, and this node has none mounted";
  // Staged counts: the bundle is whole on the card and the installer is on its
  // way to open it. Letting a second upload in here deletes that file out from
  // under the task about to read it, which is how a good update goes missing.
  if (current == Stage::Receiving || current == Stage::Staged || current == Stage::Installing)
    return "an update is already in progress";
  if (current == Stage::Installed)
    return "an update is installed; the node is restarting into it";
  return nullptr;
}

// What the portal and the API report.
struct Progress {
  Stage    stage    = Stage::Idle;
  uint32_t received = 0;
  uint32_t expected = 0;
  char     message[112] = "";
};

}  // namespace Ota
