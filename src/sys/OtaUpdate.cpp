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

#include "OtaUpdate.h"

#include <Arduino.h>
// Where an update waits while it arrives. A board with a card slot stages on
// the card, beside the node's other files and off the flash the node is about
// to rewrite. A board without one stages on LittleFS — the V4 has 7.8 MB of
// it against a 1.7 MB firmware, and a slotless board with room to spare should
// not be the one board that cannot take its own updates.
#if HAS_SD
#include <SD.h>
#else
#include <LittleFS.h>
#endif

#include "Bootloader.h"
#include "Config.h"
#include "Diag.h"
#include "OtaDevice.h"
#include "SdCard.h"

namespace Ota {
namespace {

// The filesystem the bundle is staged on, asked once so every use below is
// the same question with the same answer.
inline fs::FS& stagingFs() {
#if HAS_SD
  return SD;
#else
  return LittleFS;
#endif
}
// Whether that filesystem is there to write on. LittleFS is mounted at boot
// and stays mounted; a card can be absent.
inline bool stagingReady() {
#if HAS_SD
  return sdCard.mounted();
#else
  return true;
#endif
}

Floor<NvsStore>* sFloor = nullptr;
Progress         sProgress;
SemaphoreHandle_t sLock = nullptr;
File             sStaging;

// Whose transfer the staging file belongs to. One at a time, and every chunk
// has to carry the token receiveStart() was given: the body handler above this
// only authenticates the first chunk of a request, so a second POST that is
// turned away at its first chunk still delivers the rest of its body, and
// without this those bytes reach receiveChunk() and tear down the transfer
// that is actually running.
const void* sOwner = nullptr;
uint32_t    sLastChunkMs = 0;

// How long a transfer may go quiet before the next caller may take the staging
// file off it. The link this arrives over is an access point in a field and an
// upload that stops half way is the ordinary case, not the strange one: the
// client simply goes, and nothing tells the node. Without this the transfer it
// can no longer receive stays nominally in progress and every later attempt is
// refused against it, so one walk out of range costs a restart.
constexpr uint32_t STALL_MS = 30000;

// Chunks arrive from the network a kilobyte or two at a time and the card
// wants them in blocks. Writing each chunk as it landed put a FATFS
// transaction and an SPI round trip behind every packet, which drained the
// upload slower than it arrived; the difference queued in the heap until an
// allocation somewhere else in the node failed and took it down — the log
// line before the crash was the AutoInterface failing a sendto with ENOMEM.
//
// So they are gathered here first. The buffer is in PSRAM because there is
// nearly two megabytes of it doing nothing and the internal heap is the thing
// under pressure.
constexpr size_t BLOCK = 32 * 1024;
uint8_t* sBlock = nullptr;
size_t   sFill  = 0;

bool flushBlock() {
  if (!sFill) return true;
  size_t done = 0;
  while (done < sFill) {
    const size_t took = sStaging.write(sBlock + done, sFill - done);
    if (took == 0) return false;
    done += took;
  }
  sFill = 0;
  return true;
}

void releaseBlock() {
  free(sBlock);
  sBlock = nullptr;
  sFill = 0;
}

void say(Stage stage, const char* message) {
  if (sLock) xSemaphoreTake(sLock, portMAX_DELAY);
  sProgress.stage = stage;
  strlcpy(sProgress.message, message ? message : "", sizeof(sProgress.message));
  if (sLock) xSemaphoreGive(sLock);
}

void countReceived(uint32_t received, uint32_t expected) {
  if (sLock) xSemaphoreTake(sLock, portMAX_DELAY);
  sProgress.received = received;
  sProgress.expected = expected;
  if (sLock) xSemaphoreGive(sLock);
}

// The image half of a staged bundle. The manifest has already been read off
// the front, so the file is sitting where the image starts and every read is
// the installer asking for more of it.
class StagedImage : public Source {
 public:
  explicit StagedImage(File& file) : _file(file) {}
  // File::read answers (size_t)-1 for a file it will not read, and the
  // installer's "longer than it declared" guard adds that to the running total
  // and wraps rather than catching it — which is a write of four gigabytes
  // from a one-kilobyte buffer. Anything that is not a count is no bytes.
  size_t read(uint8_t* buf, size_t max) override {
    const size_t got = _file.read(buf, max);
    return got > max ? 0 : got;
  }
 private:
  File& _file;
};

// The install itself. Wrapped by installTask() below, which is what the task
// actually runs: this reads a couple of megabytes off a card, hashes them and
// writes a partition, and every one of those allocates. Diag.h's rule is that
// every task this firmware starts is guarded — an allocation failure here
// would otherwise abort the node in the middle of an install rather than
// report that the install failed.
void runInstall() {
  say(Stage::Installing, "reading the staged update");

  File bundle = stagingFs().open(STAGING_PATH, FILE_READ);
  if (!bundle) {
    say(Stage::Failed, "the staged update could not be reopened");
    return;
  }

  uint8_t manifest[FirmwareManifest::SIZE];
  const bool haveManifest = bundle.read(manifest, sizeof(manifest)) == sizeof(manifest);
  if (!haveManifest) {
    bundle.close();
    say(Stage::Failed, "the file is too short to be an update");
    return;
  }

  EspTarget target;
  DeviceInstaller installer(target, *sFloor);
  StagedImage image(bundle);
  const Outcome out = installer.install(manifest, sizeof(manifest), image);
  bundle.close();

  char line[112];
  if (out.result == Install::Ok) {
    snprintf(line, sizeof(line), "installed %s (version %lu); restarting into it",
             out.manifest.version, (unsigned long)out.manifest.secureVersion);
    say(Stage::Installed, line);
    log_w("update: %s", line);
    sdCard.log("update: installed, restarting into it");
    // The staged copy has done its job. Leaving it would install itself again
    // on the next upload that failed before it replaced the file.
    stagingFs().remove(STAGING_PATH);
    // A refused restart is not a failed install — the boot slot has already
    // been switched, so the update runs at the next restart whenever it comes.
    // Saying so is the difference between an operator power-cycling the node
    // and an operator waiting for a restart that was never armed.
    if (!Bootloader::reboot(Bootloader::Source::Http))
      say(Stage::Installed, "installed, but the node could not restart itself; "
                            "restart it to run the update");
  } else {
    // Both halves: what the install thought, and — when the manifest is what
    // it refused — which of its checks said no. "refused" on its own sends an
    // operator to re-upload the same file.
    if (out.result == Install::Refused)
      snprintf(line, sizeof(line), "refused: %s", FirmwareManifest::describe(out.manifestResult));
    else
      snprintf(line, sizeof(line), "%s", describe(out.result));
    say(Stage::Failed, line);
    log_e("update: %s", line);
    char logged[128];
    snprintf(logged, sizeof(logged), "update: %s", line);
    sdCard.log(logged);
  }
}

void installTask(void*) {
  // A throw would otherwise leave the stage at Installing, which every later
  // upload is refused against — the node would never take another update.
  if (!Diag::guard("the update install", runInstall))
    say(Stage::Failed, "the install was abandoned; the node is unchanged");
  // What the install actually cost. Ed25519, SHA-256, the installer's buffer
  // and FATFS all share this stack, and 8 KiB was an estimate — an estimate
  // that fails does so while a partition is half written, so the figure is
  // reported rather than assumed.
  const uint32_t spare = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  if (spare < 1024) log_w("update: the install task finished with only %lu bytes of stack to spare",
                          (unsigned long)spare);
  else              log_i("update: the install task finished with %lu bytes of stack to spare",
                          (unsigned long)spare);
  vTaskDelete(nullptr);
}

}  // namespace

void begin(Floor<NvsStore>& floor) {
  sFloor = &floor;
  if (!sLock) sLock = xSemaphoreCreateMutex();
  // A bundle left by a transfer that died, or by a node that lost power
  // between staging and installing. It is not resumed: a partial file would
  // fail the length check anyway, and a whole one that was never asked for
  // should not install itself because the node happened to reboot.
  if (stagingReady() && stagingFs().exists(STAGING_PATH)) {
    stagingFs().remove(STAGING_PATH);
    log_i("update: cleared a staged update left over from a previous run");
  }
}

uint32_t acceptedFloor() { return sFloor ? sFloor->accepted() : 0; }

const char* runningSlot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  return running ? running->label : "unknown";
}

Progress progress() {
  // Without the lock the state is still the state — say() and countReceived()
  // write it unlocked in that case too. Answering a blank Progress instead
  // would have receiveChunk() compare every index against a received count of
  // zero and refuse the second chunk of every upload.
  Progress copy;
  if (sLock) {
    xSemaphoreTake(sLock, portMAX_DELAY);
    copy = sProgress;
    xSemaphoreGive(sLock);
  } else {
    copy = sProgress;
  }
  // A transfer nobody is feeding any more is not one in progress. Reported
  // rather than torn down here — this is a getter, and the teardown belongs to
  // the next receiveStart() — but reported, because the portal draws its button
  // from this and would otherwise sit disabled for ever against an upload that
  // is never going to arrive. The byte counts are left alone: receiveChunk()
  // matches every chunk against `received`.
  if (copy.stage == Stage::Receiving && millis() - sLastChunkMs > STALL_MS) {
    copy.stage = Stage::Failed;
    strlcpy(copy.message, "the upload stopped arriving", sizeof(copy.message));
  }
  return copy;
}

// Everything a transfer holds, put down in one place: the file, the block
// buffer and the token that says whose it is. Called from every path that ends
// one, so no failure can leave half of it behind.
namespace {
void abandon(bool removeFile) {
  if (sStaging) sStaging.close();
  if (removeFile) stagingFs().remove(STAGING_PATH);
  releaseBlock();
  sOwner = nullptr;
}
}  // namespace

const char* receiveStart(uint32_t totalBytes, const void* owner) {
  // First, and without touching the progress this reports: a transfer already
  // running owns that state, and answering a second caller by overwriting it
  // makes the node report the wrong thing about the wrong upload.
  if (sOwner && millis() - sLastChunkMs > STALL_MS) {
    abandon(true);
    say(Stage::Failed, "the previous upload stopped arriving and was abandoned");
  }
  if (sOwner) return "an update is already in progress";
  if (!sFloor) return "the node is still starting up";
  if (const char* why = uploadRefusal(stagingReady(), canSelfUpdate(), progress().stage)) {
    say(Stage::Failed, why);
    return why;
  }
  if (totalBytes <= FirmwareManifest::SIZE) {
    const char* why = "the file is too short to be an update";
    say(Stage::Failed, why);
    return why;
  }
  // What the slot can hold, asked before a byte is written rather than by the
  // manifest check two megabytes later. A body that cannot fit the partition
  // is not going to become installable by arriving in full, and the card it
  // would have filled is the one the Reticulum store lives on.
  const uint32_t slot = EspTarget().slotSize();
  if (slot && totalBytes > (uint32_t)FirmwareManifest::SIZE + slot) {
    const char* why = "the file is larger than the slot it would be written to";
    say(Stage::Failed, why);
    return why;
  }
  stagingFs().mkdir(STAGING_DIR);
  stagingFs().remove(STAGING_PATH);
  sStaging = stagingFs().open(STAGING_PATH, FILE_WRITE);
  if (!sStaging) {
    const char* why = "the staging storage would not take the upload";
    say(Stage::Failed, why);
    return why;
  }
  releaseBlock();
  sBlock = (uint8_t*)ps_malloc(BLOCK);
  if (!sBlock) sBlock = (uint8_t*)malloc(BLOCK);   // no PSRAM on this board
  if (!sBlock) {
    abandon(true);
    const char* why = "there was not enough memory to receive an update";
    say(Stage::Failed, why);
    return why;
  }
  sOwner = owner;
  sLastChunkMs = millis();
  countReceived(0, totalBytes);
  say(Stage::Receiving, "receiving");
  return nullptr;
}

bool receiveChunk(const void* owner, uint32_t index, const uint8_t* data, size_t len) {
  // Not this transfer's bytes. Dropped rather than refused: the body of a POST
  // that was turned away keeps arriving whatever this returns, and the one
  // thing it must not do is disturb the transfer that is running.
  if (owner != sOwner || !sStaging || !sBlock) return false;
  sLastChunkMs = millis();
  // The four bytes a manifest starts with, on the first chunk. A raw
  // firmware.bin is a plausible thing to pick out of a build directory and is
  // refused by the installer anyway — but only after somebody has stood under
  // a pole watching two megabytes go up a one-bar link.
  if (index == 0 && len >= sizeof(FirmwareManifest::MAGIC) &&
      memcmp(data, FirmwareManifest::MAGIC, sizeof(FirmwareManifest::MAGIC)) != 0) {
    abandon(true);
    say(Stage::Failed, "that is not an update bundle: it does not begin with a manifest "
                       "(tools/fw_sign.py bundle makes one)");
    return false;
  }
  // Where this chunk says it belongs, against where the file has got to. They
  // agree for a body that arrives once, in order, from one client. Anything
  // else is not an upload this node can assemble, and carrying on would leave
  // a plausible-looking file that is two uploads interleaved.
  const uint32_t received = progress().received;
  if (index != received) {
    char why[112];
    snprintf(why, sizeof(why), "the upload arrived out of order (%lu where %lu was expected)",
             (unsigned long)index, (unsigned long)received);
    abandon(true);
    say(Stage::Failed, why);
    return false;
  }
  const size_t arrived = len;      // the loop below consumes both operands
  while (len) {
    const size_t room = BLOCK - sFill;
    const size_t take = len < room ? len : room;
    memcpy(sBlock + sFill, data, take);
    sFill += take;
    data += take;
    len  -= take;
    if (sFill == BLOCK && !flushBlock()) {
      char why[112];
      snprintf(why, sizeof(why), "the storage stopped accepting the upload at %lu bytes",
               (unsigned long)received);
      abandon(true);
      say(Stage::Failed, why);
      return false;
    }
  }
  countReceived(received + (uint32_t)arrived, progress().expected);
  return true;
}

bool receiveEnd(const void* owner) {
  if (owner != sOwner || !sStaging) return false;
  const bool flushed = flushBlock();
  abandon(!flushed);
  if (!flushed) {
    say(Stage::Failed, "the last of the upload would not go onto the storage");
    return false;
  }
  say(Stage::Staged, "staged; verifying");
  // Off the web server's task: this reads a couple of megabytes off the card,
  // hashes them and writes a partition, and the task it arrived on has other
  // sockets waiting on it.
  if (!Diag::startTask(installTask, "ota", 8192, nullptr, 1, 1)) {
    stagingFs().remove(STAGING_PATH);
    say(Stage::Failed, "there was not enough memory to start the install");
    return false;
  }
  return true;
}

}  // namespace Ota
