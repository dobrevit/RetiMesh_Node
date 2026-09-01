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
//  to one question and no way to tell which is right. So a move is a move:
//  the copy is checked in its new home before the old tree is deleted, and
//  when it is over there is one tree and not two.
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
//  A marker that cannot be read is not the same as no marker at all. The file
//  is opened for writing, which empties it before anything is serialised into
//  it, so a power cut while it is written leaves nothing behind but a name —
//  and a node that reads "no marker" off another node's card, finds a store
//  beside it and calls it legacy, adopts that card and signs it. The card is
//  then taken for good. So an unreadable claim is treated as somebody else's:
//  a card this node will not touch until a human formats it.
//
//  And a released card is free
//  ---------------------------
//  Ejecting takes the store off the card and writes "released" into the
//  marker. The card still names the node that owned it, which is worth having
//  for a human holding it, but the flag is that node saying it is finished
//  with the card. Any node may take it from there. While only the identity
//  was compared, an ejected card stayed foreign for ever: the node that
//  released it could pick it up again because the name matched, and every
//  other node had to format a card whose owner had already given it up.
//
//  The move happens at boot, with nothing open
//  -------------------------------------------
//  The stores underneath belong to the transport library, hold their files
//  open for their lifetime and buffer writes in RAM, so moving them while the
//  node is routing means closing and reopening three of them mid-flight
//  underneath the task that is writing to them. So a request moves nothing.
//  It records what was asked for and restarts the node, and the move is made
//  on the way back up, before the transport opens the store: nothing is open,
//  nothing else is writing, and the home is then chosen by the same rule as
//  always.
//
//  Which is also why a blank card is not a home yet. The setting saying the
//  store belongs on a card is not the same as the card holding the store, and
//  a node that opens an empty store on a blank card — with its real path
//  table still in flash — is indistinguishable from a node that has lost its
//  data. So a blank card is taken the same way an adopt takes one: copy
//  first, then live there.
// ============================================================================
#pragma once

// Deliberately free of Arduino.h and Config.h. The rules below decide where a
// node's data lives, which makes them worth testing, and a rule that can only
// be exercised by flashing a board with a card in it will not be.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace StoreHome {

// Where the store is open.
enum class Where : uint8_t { LittleFs, Sd };

// What the card in the slot holds, as far as ownership goes. This is the
// question that decides whether taking it is safe.
enum class Card : uint8_t {
  NoCard,    // nothing mounted
  Blank,     // mounted, no store of ours on it: free to take, once copied to
  Ours,      // marked with this node's identity
  Foreign,   // marked with a different node's identity: leave it alone
  Legacy,    // a store but no marker, from before markers existed
};

// What reading the card's marker produced. Absent and Unreadable are opposite
// answers — nobody has claimed this card, versus somebody has and the claim
// cannot be checked — and a bool cannot tell them apart, which is how a
// truncated marker on another node's card came to look like a card of ours.
enum class MarkerState : uint8_t { Absent, Read, Unreadable };

// A move that has been asked for and not made yet. It outlives the request —
// the node restarts in between — so it is persisted with the settings.
enum class Move : uint8_t { None = 0, Adopt = 1, Eject = 2 };

// The identity hex written into the marker is two characters per byte of a
// Reticulum hash. This header cannot see Rns::HASH_LEN without dragging
// Arduino in, so StoreHome.cpp — where both are visible — static_asserts the
// two against each other, and this is the number the host tests can reach.
static const size_t kIdentityHexLen = 32;

struct Marker {
  char     node[41]   = "";   // identity hex of the node that owns the store
  char     name[33]   = "";   // its node name, so a human reading the card knows
  uint32_t generation = 0;    // bumped every time the store moves
  bool     released   = false;// synced away by an eject; present but not in use
  bool     valid      = false;
};

// What the slot holds, as last read by the card task. The status page asks on
// every refresh and used to be answered by opening and parsing the marker file
// then and there — several times per request, on the web server's task, on a
// bus the card task was using at the same time.
struct Ownership {
  Card     card       = Card::NoCard;
  char     owner[33]  = "";   // the marker's node name, for the operator
  uint32_t generation = 0;
};

// --- the rules --------------------------------------------------------------
// Pure, so they can be tested without a filesystem, and so there is one
// statement of each rather than one per caller. Where the store belongs used
// to be written inline where the transport starts, which is fine until
// something else needs the same answer.

// What the card holds, from the facts that can be read off it.
inline Card classify(bool mounted, MarkerState marker, bool markerIsOurs,
                     bool markerReleased, bool storePresent) {
  if (!mounted) return Card::NoCard;
  if (marker == MarkerState::Read) {
    // Released outranks the name on the card: it is the owner's own note that
    // it has taken its store back and is done here. Whatever is left behind on
    // a released card is a leftover, not a live store.
    if (markerReleased) return Card::Blank;
    return markerIsOurs ? Card::Ours : Card::Foreign;
  }
  // A marker that is there and unreadable is a claim, and a claim this node
  // cannot check is not a claim it may overrule. Foreign is the conservative
  // reading: the card is left alone, and a human who knows it is theirs can
  // still format it. Reading it as ours would sign somebody else's card on the
  // strength of a file that says nothing.
  if (marker == MarkerState::Unreadable) return Card::Foreign;
  // No marker at all. A store already on the card was put there by this node
  // before markers existed — a card does not acquire one by accident — so it
  // is ours.
  return storePresent ? Card::Legacy : Card::Blank;
}

// Where the store belongs, given the setting and what the card holds.
inline Where decide(bool sdStoreSetting, bool cardMounted, Card c) {
  if (!sdStoreSetting || !cardMounted) return Where::LittleFs;
  // Only a card that is already carrying this node's store is a home. A card
  // holding another node's store is not ours to answer for — adopting it would
  // mean routing from that node's path table and overwriting its data on the
  // first write, and whoever moved the card almost certainly meant to move it,
  // not to merge it. A blank one is not refused but not ready: the store has
  // to be copied onto it first, which is what planAtBoot arranges.
  return (c == Card::Ours || c == Card::Legacy) ? Where::Sd : Where::LittleFs;
}

// Whether this node may take this card: there has to be one, and it must not
// be carrying somebody else's store. The page that offers the action and the
// boot that carries it out both ask here, so they cannot come to different
// conclusions about whether it is even possible.
inline bool adoptable(Card c) { return c != Card::NoCard && c != Card::Foreign; }

// What has to be copied before the store is opened: the move the operator
// asked for, if the card in the slot can still take it, or the one a card the
// setting wants to use needs before it can be a home at all.
//
// Every answer is measured against where the store already is — decide()'s
// answer for this same card, from these same facts, rather than a runtime flag
// saying which filesystem is currently pointed at. The two came apart when the
// home was chosen inside a transport that had been switched off: the flag said
// flash while the store sat on the card, the page offered to adopt a card that
// was already the home, and the boot after that copied the flash tree over the
// real one. A move to where the store already is has nothing to copy, so it is
// never planned however stale anything else's idea of the home has become.
inline Move planAtBoot(Move requested, bool sdStoreSetting, bool cardMounted, Card c) {
  const bool  usable = cardMounted && adoptable(c);
  const Where home   = decide(sdStoreSetting, cardMounted, c);
  if (requested == Move::Adopt) return (usable && home != Where::Sd) ? Move::Adopt : Move::None;
  // An eject reads the store off the card, so the card has to be the home it
  // is read from.
  if (requested == Move::Eject) return (usable && home == Where::Sd) ? Move::Eject : Move::None;
  return (sdStoreSetting && cardMounted && c == Card::Blank) ? Move::Adopt : Move::None;
}

// Why a move cannot be offered, or nullptr when it can. One statement of each
// rule, in the words the operator reads: the page draws its buttons from it and
// the request refuses with it, so a button is never lit for a move that will be
// turned down. Eject used to be offered on "the store is on the card" alone,
// which stays true after the card has been pulled — the eject it queued cost a
// restart and then failed for want of a card to read the store off.
inline const char* ejectRefusal(bool moveQueued, Where current, bool cardLost) {
  if (moveQueued)           return "a move is already queued";
  if (current != Where::Sd) return "the store is not on the card";
  if (cardLost)             return "the card was removed; restart the node before moving the store";
  return nullptr;
}

// `cardNamesThisNode` separates the two ways a card comes back foreign. A card
// written by a different node is the case the rule is for. A card written by
// *this* board under an earlier identity — after a flash erase, say — reads
// foreign too, because ownership is the identity and not the name, and the
// operator then gets told the card belongs to the node they are standing in
// front of. Same refusal, and it has to stay the same refusal: the store on
// that card cannot be opened by an identity that did not write it. But saying
// which of the two it is costs a sentence and saves an afternoon.
inline const char* adoptRefusal(bool moveQueued, Where current, Card c, bool cardLost,
                                bool cardNamesThisNode = false) {
  if (moveQueued)           return "a move is already queued";
  if (cardLost)             return "the card was removed; restart the node before moving the store";
  if (c == Card::NoCard)    return "no card";
  if (current == Where::Sd) return "the store is already on the card";
  // The owner's name is not something a pure rule can see; requestAdopt() adds
  // it to this sentence rather than writing a second one.
  if (c == Card::Foreign)
    return cardNamesThisNode
      ? "refused: this card was written by an earlier identity of this node, which cannot be "
        "read from here; format it if you mean to take it"
      : "refused: this card holds another node's store; format it first if you mean to take it";
  return nullptr;
}

// Why the store is not on the card, for the one line a node says about it at
// boot. Null when it is on the card and there is nothing to explain.
//
// Pure, so the wording of each case is settled and tested here rather than at
// the single call site, which can only be reached by booting a board with a
// card in the slot — which is exactly how a node came to sit quietly in 128 KiB
// of flash with eight gigabytes plugged into it.
inline const char* flashReason(bool sdStoreSetting, bool cardMounted, Card c) {
  if (decide(sdStoreSetting, cardMounted, c) == Where::Sd) return nullptr;
  if (!cardMounted)       return "there is no card in the slot";
  if (!sdStoreSetting)    return "the settings keep the store off the card";
  if (c == Card::Foreign) return "the card holds a store this node does not own; format it to take the card";
  return "the card is not carrying this node's store yet";
}

// --- the node --------------------------------------------------------------
//
// Both of the boot calls below drive the card directly, for seconds at a time
// in the case of a move, and neither may share the bus with the card task. The
// task is therefore not started until they are done — see main.cpp, which
// mounts the card, moves the store, chooses the home and only then lets the
// slot be polled. A flag read at the top of the poll cannot help here: it stops
// the next poll, not the one already inside a removal check, and the answer to
// a removal check that loses the bus to a copy is unmount(), which frees the
// card struct under the task doing the copying.
void  begin();                // before the card task starts: the ownership cache

// Carries out a queued move. Call at boot before anything opens the store,
// and before chooseAtBoot, which then finds the home already made.
void  runPendingMigration();

// The rule applied to this node, here, now: points the store's filesystem at
// the home it picks and returns it. Call once at boot, from setup() and not
// from whatever happens to open the store first — this used to live inside the
// transport's start-up, behind its enabled check, so a node with the transport
// switched off never chose at all and every answer about the store's home was
// the default one: flash, even with the store sitting on the card.
Where chooseAtBoot();

Where       where();          // where the store is open right now
Card        card();           // what the inserted card holds
Ownership   ownership();      // ... with the owner's name, for the UI
const char* lastResult();     // outcome of the last move, for the UI
bool        busy();           // a move is queued for the next boot, or running

// Requests. Both record the move, schedule a restart and answer immediately;
// nothing is copied until the node is on its way back up. False means refused,
// and lastResult() says why — the callers relay that rather than restating the
// rule, so that there is one place where a move can be turned down.
bool requestAdopt();          // LittleFS -> SD, at the next boot
bool requestEject();          // SD -> LittleFS, after which the card is free

// Whether each move can be offered at all, asked of this node as it stands.
// The pages publish their buttons from these, so a button and the request
// behind it cannot come to different conclusions.
bool canAdopt();
bool canEject();

// Re-reads the card's marker. Called from the card task, which is the task
// that owns the card, when something has happened that can have changed it: a
// card arriving or going away, a format, a move, a marker rewritten. It used to
// run on every poll — an open, a JSON parse and a directory probe every three
// seconds, for ever, on the bus the store is using — to save having to remember
// the occasions on which it matters. Those occasions all pass through this
// file or the card task, and there are five of them.
void refreshCache();

const char* whereName(Where w);
const char* cardName(Card c);

} // namespace StoreHome
