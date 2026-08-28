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
//  StoreHome.h — where the Reticulum store lives, and how it moves
//
//  The store has exactly one home at a time: the SD card when there is one to
//  use, the LittleFS partition otherwise. Never both. Two live copies of the
//  same path table, each being written, is not redundancy — it is two answers
//  to one question and no way to tell which is right.
//
//  Adopting a card used to mean editing a setting and rebooting by hand, and
//  the store that was already on the internal flash simply stayed behind. This
//  moves it instead: the data is copied, the card is marked, and the node
//  restarts into its new home. A restart is still involved, but it is one
//  click and the node arranges it, which is a different thing from asking an
//  operator to do the dance.
//
//  Why the card carries a marker
//  -----------------------------
//  A card holding a store looks exactly like a card holding somebody else's
//  store. Without a name on it, a node that finds a populated card can either
//  adopt it — and inherit another node's identity of record — or ignore it,
//  and lose its own data. Neither is right, because the question is not
//  answerable from the files. So the owner writes its name on the card, and
//  from then on the question is answerable: mine, someone else's, or blank.
//
//  A store written before markers existed has no name on it. That is not
//  ambiguous in practice — it is on a card in a node that was already using
//  it — so it counts as ours and gets a marker on the next mount.
//
//  Restart, not runtime swap
//  -------------------------
//  The stores underneath belong to the transport library and hold their files
//  open for their lifetime, so re-pointing them while the node is routing
//  means closing and reopening three of them mid-flight. That is a real
//  option and it is what a surprise removal will eventually need, since you
//  cannot reboot your way out of a card that has already gone. A deliberate
//  migration does not need it: the node is about to restart anyway, so the
//  copy happens with nothing else writing and the new home is chosen at boot
//  by the same rule as always.
// ============================================================================
#pragma once

// Deliberately free of Arduino.h and Config.h. The rule below decides where a
// node's data lives, which makes it worth testing, and a rule that can only be
// exercised by flashing a board with a card in it will not be.
#include <stdint.h>
#include <string.h>

namespace StoreHome {

// Where the store is open.
enum class Where : uint8_t { LittleFs, Sd };

// What the card in the slot holds, as far as ownership goes. This is the
// question that decides whether taking it is safe.
enum class Card : uint8_t {
  NoCard,    // nothing mounted
  Blank,     // mounted, no store and no marker: free to take
  Ours,      // marked with this node's identity
  Foreign,   // marked with a different node's identity: leave it alone
  Legacy,    // a store but no marker, from before markers existed
};

struct Marker {
  char     node[41]   = "";   // identity hex of the node that owns the store
  char     name[33]   = "";   // its node name, so a human reading the card knows
  uint32_t generation = 0;    // bumped every time the store moves
  bool     released   = false;// synced away by an eject; present but not in use
  bool     valid      = false;
};

// --- the rule ---------------------------------------------------------------
// Where the store belongs, given the setting, whether a card is mounted, and
// what that card holds. Pure, so the rule can be tested without a filesystem,
// and so there is one statement of it rather than one per caller. It used to
// be written inline where the transport starts, which is fine until something
// else needs the same answer.
inline Where decide(bool sdStoreSetting, bool cardMounted, Card c) {
  if (!sdStoreSetting || !cardMounted) return Where::LittleFs;
  // A card carrying another node's store is the one case where the setting
  // does not get its way. Adopting it would mean answering to that node's path
  // table and overwriting its data on the first write, and whoever moved the
  // card almost certainly meant to move it, not to merge it.
  if (c == Card::Foreign) return Where::LittleFs;
  return Where::Sd;
}

// The same rule applied to this node, here, now. Call at boot to pick the
// backing filesystem.
Where chooseAtBoot();

Where       where();          // where the store is open right now
Card        card();           // what the inserted card holds
bool        readMarker(Marker& out);
const char* lastResult();     // outcome of the last migration, for the UI
bool        busy();           // a migration is queued or running

// Requests. Both answer immediately and are carried out by service() on the
// RNS task, which is the only task that writes to the store — so a migration
// never races the thing it is migrating.
bool requestAdopt();          // LittleFS -> SD, then restart
bool requestEject();          // SD -> LittleFS, then restart, card safe to pull

void service();               // called from RnsTransport::loop()

const char* whereName(Where w);
const char* cardName(Card c);

} // namespace StoreHome
