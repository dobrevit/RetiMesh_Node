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
//  OtaUpdate.h — taking an update from whoever is holding it
//
//  The operator case this is for: a node on a pole, an afternoon's drive away,
//  and somebody standing under it with a phone. They join its access point,
//  open the portal and hand it a file. Nothing to install on their side, no
//  server to run, and no cable — which is the whole point, because there will
//  not be one.
//
//  The file is a bundle: the 284-byte manifest, then the image. One file
//  rather than two, because two files is a way to install a manifest with the
//  wrong image beside it, and nothing in a downloads folder says which goes
//  with which. Concatenated, the question cannot be asked.
//
//  Why it lands on the card first
//  ------------------------------
//  The installer pulls its bytes and the web server pushes them, and an
//  adapter between the two — a queue, a semaphore and a second task — is a bug
//  farm guarding the one path that has to work. Staging turns the push back
//  into a pull for the price of a file.
//
//  It does not get the writes off the async task: a block is flushed to the
//  card from the task the chunks arrive on, which blocks it for the length of
//  a 32 KiB write. That is deliberate rather than overlooked. Blocking there
//  is what stops the sender getting ahead of the card — the version that did
//  not block queued the difference in the heap until an allocation elsewhere
//  failed and took the node down — and it is the same backpressure a queue
//  would have to reimplement, minus the queue. It also means a transfer that dies half way costs the upload and
//  nothing else: the slot has not been touched, because nothing is written to
//  flash until the manifest has been judged and the whole image is in hand.
//
//  The cost is a card. A board without one cannot take an update this way and
//  says so before the upload rather than after it.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaDevice.h"      // NvsStore, EspTarget: this half needs the hardware
#include "OtaUpdatePlan.h"  // Stage, Progress, uploadRefusal: the half that does not

namespace Ota {

// Called from setup(), after the card is mounted and the floor is open.
void begin(Floor<NvsStore>& floor);

Progress progress();

// The anti-rollback floor this node has reached, and which of the two app
// slots it is running from. Neither is visible any other way: the floor lives
// in NVS and the slot is a partition address, and an operator deciding what to
// sign next needs to know the first while somebody proving an update took
// needs the second.
uint32_t acceptedFloor();
const char* runningSlot();

// Driven by whatever is holding the bytes — today the portal's POST handler.
//
// `owner` identifies the transfer: any pointer the caller keeps for as long as
// the body lasts and does not reuse, which for the portal is the request. One
// transfer owns the staging file at a time and chunks that do not carry its
// token are dropped, because the alternative is what the index check below was
// written to catch after the fact. A second POST — a retry, another tab, or
// somebody on the access point with no password who cannot get past
// receiveStart() but whose body keeps arriving anyway — must not be able to
// reach into the transfer already running.
//
// receiveStart() returns nullptr when the upload may begin, otherwise the
// sentence to answer with; a false from the other two leaves the reason in
// progress().message. A refusal never touches a transfer already in flight —
// except one that has stopped arriving, which receiveStart() takes over: a
// client that walks out of range simply stops, nothing tells the node, and
// without that the node refuses every later attempt against a transfer it can
// no longer receive.
const char* receiveStart(uint32_t totalBytes, const void* owner);
// `index` is where this chunk belongs in the body. It is checked rather than
// assumed: a browser that retries, a connection that restarts, or two clients
// posting at once all arrive as chunks whose index does not follow the last,
// and appending them in arrival order would write a file that is neither
// upload — with a byte count that overshoots the size it was promised, which
// is how this was noticed.
bool receiveChunk(const void* owner, uint32_t index, const uint8_t* data, size_t len);
bool receiveEnd(const void* owner);   // hands the staged bundle to the installer, off this task

// Where a bundle waits on the card, beside the node's other files. The
// directory is named once and the file is named under it, so the mkdir and the
// open cannot end up pointing at different places.
inline constexpr const char* STAGING_DIR  = "/retimesh";
inline constexpr const char* STAGING_PATH = "/retimesh/update.rmfw";

}  // namespace Ota
