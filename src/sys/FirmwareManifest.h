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
//  FirmwareManifest.h — deciding whether an image may be installed
//
//  A node that installs its own updates is a node that executes whatever it is
//  persuaded to accept, so the question this answers is the whole of the
//  security of over-the-air updates: was this image produced by someone allowed
//  to produce it, is it meant for this board, and is it newer than what we have
//  already agreed to run.
//
//  The shape is the one TLS and GPG use, cut down to what a device with no
//  clock and no network can check:
//
//    two root public keys, compiled in, private halves offline
//      -> a delegation, signed by a root, naming a signing key
//         -> a manifest, signed by that key, describing one image
//
//  Delegations are what make signing keys rotatable without a firmware update,
//  which is why CI is given one and never a root. There are no expiry dates
//  anywhere in this file: expiry needs a clock, and this hardware does not have
//  one that survives a flat battery. Freshness is a monotonic version floor
//  instead, which needs neither a clock nor a network.
//
//  Everything here is parsing and policy — pure, no Arduino, no crypto. The
//  signature check is a function the caller supplies, because the bugs live in
//  the ordering and the field checks rather than in Ed25519, and because that
//  is what lets the whole policy be tested on a host that has no Ed25519 at
//  all. The device passes the real one; the tests pass a fake that records what
//  it was asked.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace FirmwareManifest {

// A fixed layout, little-endian, no length prefixes and nothing optional: the
// device parses this before it trusts any of it, so there is nothing to get
// wrong. 284 bytes, published beside the image.
//
//   [0  .. 100)  image record    — signed by the delegate
//   [100..156)  delegation record — signed by a root
//   [156..220)  root signature over the delegation record
//   [220..284)  delegate signature over the image record
static const size_t IMAGE_RECORD_OFFSET      = 0;
static const size_t IMAGE_RECORD_SIZE        = 100;
static const size_t DELEGATION_RECORD_OFFSET = 100;
static const size_t DELEGATION_RECORD_SIZE   = 56;
static const size_t ROOT_SIG_OFFSET          = 156;
static const size_t DELEGATE_SIG_OFFSET      = 220;
static const size_t SIGNATURE_SIZE           = 64;
static const size_t KEY_SIZE                 = 32;
static const size_t HASH_SIZE                = 32;
static const size_t SIZE                     = 284;

static const uint8_t  MAGIC[4]       = { 'R', 'M', 'F', 'W' };
static const uint8_t  FORMAT_VERSION = 1;
static const uint32_t PURPOSE_FIRMWARE = 1u << 0;   // this delegate may sign firmware

static const size_t BOARD_LEN   = 16;    // "t3s3", NUL-padded
static const size_t VERSION_LEN = 32;    // "v0.0.9-40-g87d8cec", NUL-padded
static const size_t LABEL_LEN   = 16;    // what the delegate is called, for the log

struct Manifest {
  uint8_t  imageHash[HASH_SIZE];
  uint32_t imageSize;
  uint32_t secureVersion;               // the anti-rollback floor this image claims
  char     board[BOARD_LEN + 1];
  char     version[VERSION_LEN + 1];
  uint32_t slotSize;                    // the app partition it was built for
  uint8_t  delegateKey[KEY_SIZE];
  uint32_t delegatePurpose;
  uint32_t delegateMinVersion;          // the lowest secure_version this delegate may sign
  char     delegateLabel[LABEL_LEN + 1];
};

enum class Result : uint8_t {
  Ok = 0,
  TooShort,           // not 284 bytes
  BadMagic,
  UnknownFormat,      // a manifest from a newer scheme than this firmware knows
  UnknownRoot,        // no compiled-in root signed this delegation
  BadDelegation,      // the delegation does not verify against the root that claims it
  NotForFirmware,     // the delegate is not permitted to sign firmware
  BadImageSignature,  // the delegate did not sign this image record
  WrongBoard,
  WrongSlotSize,
  BelowDelegateFloor, // the delegate may not sign a version this low
  Rollback,           // older than what this node has already accepted
  Oversize,           // will not fit the partition it would be written to
};

inline const char* describe(Result r) {
  switch (r) {
    case Result::Ok:                 return "ok";
    case Result::TooShort:           return "manifest is too short";
    case Result::BadMagic:           return "not a firmware manifest";
    case Result::UnknownFormat:      return "manifest format is newer than this firmware";
    case Result::UnknownRoot:        return "signed by a root this firmware does not trust";
    case Result::BadDelegation:      return "the delegation does not verify";
    case Result::NotForFirmware:     return "that key is not permitted to sign firmware";
    case Result::BadImageSignature:  return "the image signature does not verify";
    case Result::WrongBoard:         return "built for another board";
    case Result::WrongSlotSize:      return "built for another partition layout";
    case Result::BelowDelegateFloor: return "below the floor that key may sign";
    case Result::Rollback:           return "older than the version already accepted";
    case Result::Oversize:           return "larger than the partition it would be written to";
  }
  return "unknown";
}

// What this node is and what it will accept. Supplied by the caller so the
// policy can be exercised on a host with no board attached.
struct Policy {
  const uint8_t (*roots)[KEY_SIZE];   // the compiled-in trust anchors
  size_t   rootCount;
  const char* board;                  // this build's env, e.g. "t3s3"
  uint32_t slotSize;                  // this board's app partition
  uint32_t acceptedVersion;           // the anti-rollback floor already reached
};

// Ed25519 on the device; on a host, whatever the test needs. Returns true when
// `sig` is a valid signature over `msg` by `key`.
using Verifier = bool (*)(const uint8_t* sig, const uint8_t* key,
                          const uint8_t* msg, size_t len);

namespace detail {
inline uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void copyField(char* out, const uint8_t* in, size_t len) {
  memcpy(out, in, len);
  out[len] = '\0';
}
}  // namespace detail

// Parse and check, in the order that refuses cheapest-first: shape, then the
// chain, then whether the image is for us, then whether it is new enough. Every
// field of `out` is filled whenever the manifest parses, even when the result
// is a refusal, so a caller can log what it turned down.
inline Result check(const uint8_t* blob, size_t len, const Policy& policy,
                    Verifier verify, Manifest& out) {
  memset(&out, 0, sizeof(out));
  if (len < SIZE) return Result::TooShort;
  if (memcmp(blob, MAGIC, sizeof(MAGIC)) != 0) return Result::BadMagic;
  if (blob[4] != FORMAT_VERSION) return Result::UnknownFormat;

  const uint8_t* image = blob + IMAGE_RECORD_OFFSET;
  const uint8_t* deleg = blob + DELEGATION_RECORD_OFFSET;
  memcpy(out.imageHash, image + 8, HASH_SIZE);
  out.imageSize     = detail::le32(image + 40);
  out.secureVersion = detail::le32(image + 44);
  detail::copyField(out.board,   image + 48, BOARD_LEN);
  detail::copyField(out.version, image + 64, VERSION_LEN);
  out.slotSize      = detail::le32(image + 96);
  memcpy(out.delegateKey, deleg, KEY_SIZE);
  out.delegatePurpose    = detail::le32(deleg + 32);
  out.delegateMinVersion = detail::le32(deleg + 36);
  detail::copyField(out.delegateLabel, deleg + 40, LABEL_LEN);

  // The chain first: an image record is not worth reading fields out of until
  // something we trust has vouched for the key that signed it.
  bool rootFound = false;
  for (size_t i = 0; i < policy.rootCount && !rootFound; i++)
    if (verify(blob + ROOT_SIG_OFFSET, policy.roots[i], deleg, DELEGATION_RECORD_SIZE))
      rootFound = true;
  if (!rootFound) return Result::UnknownRoot;
  if ((out.delegatePurpose & PURPOSE_FIRMWARE) == 0) return Result::NotForFirmware;
  if (!verify(blob + DELEGATE_SIG_OFFSET, out.delegateKey, image, IMAGE_RECORD_SIZE))
    return Result::BadImageSignature;

  // Then whether this image is for this node at all.
  if (strncmp(out.board, policy.board, BOARD_LEN) != 0) return Result::WrongBoard;
  if (out.slotSize != policy.slotSize) return Result::WrongSlotSize;
  if (out.imageSize > policy.slotSize) return Result::Oversize;

  // Then freshness, which is a floor rather than a date because this hardware
  // has no clock that survives a flat battery.
  if (out.secureVersion < out.delegateMinVersion) return Result::BelowDelegateFloor;
  if (out.secureVersion < policy.acceptedVersion) return Result::Rollback;
  return Result::Ok;
}

}  // namespace FirmwareManifest
