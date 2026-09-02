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
//  OtaInstaller.h — writing an update without being able to take it back
//
//  The order is the whole of it, so the order is what this file exists to fix
//  in one place and what its tests assert:
//
//    1. judge the manifest      — before a byte is written, so a refusal costs
//                                 nothing and an unsigned image never touches
//                                 flash
//    2. write into the far slot — never the running one; a failure here leaves
//                                 the node exactly as it was
//    3. hash what was written   — read back from flash, not from the bytes that
//                                 arrived. A source that lies, a wire that
//                                 flips a bit and a partition that did not take
//                                 the write are the same failure to a node that
//                                 only hashed its input
//    4. switch otadata          — the first irreversible step, and it happens
//                                 only if 1-3 all passed
//    5. stage the version       — for the boot that follows to settle
//
//  What makes step 4 survivable is the bootloader's rollback: the switched-to
//  image boots as PENDING_VERIFY and the bootloader puts the old one back
//  unless the new one marks itself healthy. So the dangerous window is not "a
//  bad image was written" but "a bad image was written and then told us it was
//  fine", which is a much smaller thing to get right.
//
//  Both flash and the source are injected, because every one of these steps
//  fails in a way that only shows up on a node in a field: a short image, a
//  write that reports success and stores something else, a switch that does not
//  take. A fake target reproduces all of them on a desk.
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <SHA256.h>

#include "FirmwareManifest.h"
#include "OtaFloor.h"
#include "OtaVerify.h"
#include "OtaProgress.h"

namespace Ota {

// Where the image bytes come from. Returns how many bytes it put in the buffer,
// 0 when there are no more, and anything short of `max` is fine — the installer
// counts rather than assuming a chunk size.
struct Source {
  virtual ~Source() {}
  virtual size_t read(uint8_t* buf, size_t max) = 0;
};

enum class Install {
  Ok,
  NoSlot,           // this board has one app partition; it cannot update itself
  Refused,          // the manifest did not pass — `manifestResult` says why
  ShortImage,       // the source ran out before the size the manifest declared
  LongImage,        // the source kept going past it
  WriteFailed,
  FinishFailed,     // esp_ota_end: the written image is not a bootable one
  ReadBackFailed,
  HashMismatch,     // what landed in flash is not what was signed
  SwitchFailed,
};

inline const char* describe(Install r) {
  switch (r) {
    case Install::Ok:             return "installed";
    case Install::NoSlot:         return "this board has no second app slot to write to";
    case Install::Refused:        return "the manifest was refused";
    case Install::ShortImage:     return "the image ended before the size it declared";
    case Install::LongImage:      return "the image was longer than it declared";
    case Install::WriteFailed:    return "the update slot would not take the write";
    case Install::FinishFailed:   return "what was written is not a bootable image";
    case Install::ReadBackFailed: return "the update slot could not be read back";
    case Install::HashMismatch:   return "what was written is not what was signed";
    case Install::SwitchFailed:   return "the boot slot would not switch";
  }
  return "unknown";
}

struct Outcome {
  Install                 result = Install::NoSlot;
  FirmwareManifest::Result manifestResult = FirmwareManifest::Result::TooShort;
  FirmwareManifest::Manifest manifest{};
  size_t                  bytesWritten = 0;
};

// A Target provides:
//   bool     haveSlot()
//   uint32_t slotSize()
//   uint32_t slotId()                      — what Floor records, to tell the
//                                            slot apart from the one we booted
//   bool     begin(size_t imageSize)
//   bool     write(const uint8_t*, size_t)
//   bool     finish()
//   bool     readBack(size_t offset, uint8_t* buf, size_t len)
//   bool     switchTo()
template <typename Target, typename Store>
class Installer {
 public:
  // The verifier is injected for the same reason FirmwareManifest injects it:
  // the bugs in this file are in the order of the steps, and a test that has to
  // produce real signatures to reach step 4 is a test of Ed25519 with the
  // ordering as an afterthought. The default is the real one, so the firmware
  // cannot get a weaker check by forgetting an argument.
  Installer(Target& target, Floor<Store>& floor,
            FirmwareManifest::Verifier verify = verifySignature)
      : _target(target), _floor(floor), _verify(verify) {}

  Outcome install(const uint8_t* manifestBlob, size_t manifestLen, Source& source) {
    Outcome out;
    // Ends the progress line on every one of the returns below; a no-op if
    // begin() was never reached.
    struct ProgressScope {
      Outcome* o;
      ~ProgressScope() { OtaProgress::end(o->result == Install::Ok); }
    } progressScope{&out};
    if (!_target.haveSlot()) { out.result = Install::NoSlot; return out; }

    // 1. Judged before anything is written. policyFor decides who is trusted
    //    and what board this is; only the slot and the floor come from here.
    const FirmwareManifest::Policy policy =
        policyFor(_target.slotSize(), _floor.accepted());
    out.manifestResult = FirmwareManifest::check(manifestBlob, manifestLen, policy,
                                                 _verify, out.manifest);
    if (out.manifestResult != FirmwareManifest::Result::Ok) {
      out.result = Install::Refused;
      return out;
    }

    // 2. Into the slot we are not running from.
    const size_t declared = out.manifest.imageSize;
    if (!_target.begin(declared)) { out.result = Install::WriteFailed; return out; }
    OtaProgress::begin((uint32_t)declared);

    uint8_t buf[CHUNK];
    size_t written = 0;
    for (;;) {
      const size_t got = source.read(buf, sizeof(buf));
      if (got == 0) break;
      if (written + got > declared) { out.result = Install::LongImage; return out; }
      if (!_target.write(buf, got)) { out.result = Install::WriteFailed; return out; }
      written += got;
      OtaProgress::step((uint32_t)written);
    }
    out.bytesWritten = written;
    if (written != declared) { out.result = Install::ShortImage; return out; }
    if (!_target.finish()) { out.result = Install::FinishFailed; return out; }

    // 3. Hashed out of flash. Hashing the bytes that arrived would prove only
    //    that the sender and the manifest agree, which is the one thing already
    //    guaranteed by the signature.
    OtaProgress::phase("verifying");
    SHA256 sha;
    for (size_t off = 0; off < declared; ) {
      const size_t n = declared - off < CHUNK ? declared - off : CHUNK;
      if (!_target.readBack(off, buf, n)) { out.result = Install::ReadBackFailed; return out; }
      sha.update(buf, n);
      off += n;
      OtaProgress::step((uint32_t)off);
    }
    uint8_t digest[FirmwareManifest::HASH_SIZE];
    sha.finalize(digest, sizeof(digest));
    if (memcmp(digest, out.manifest.imageHash, sizeof(digest)) != 0) {
      out.result = Install::HashMismatch;
      return out;
    }

    // 4. The irreversible one.
    if (!_target.switchTo()) { out.result = Install::SwitchFailed; return out; }

    // 5. After the switch, on purpose. Losing power between the two leaves an
    //    image that boots and a floor that did not move — the update still
    //    happened, it just is not credited, which is recoverable. The other
    //    order records a floor for an image that may never run.
    _floor.stage(out.manifest.secureVersion, _target.slotId());

    out.result = Install::Ok;
    return out;
  }

 private:
  // Big enough that the per-call overhead does not dominate, small enough to
  // sit on the stack of the task doing the install.
  static constexpr size_t CHUNK = 1024;

  Target&                    _target;
  Floor<Store>&              _floor;
  FirmwareManifest::Verifier _verify;
};

}  // namespace Ota
