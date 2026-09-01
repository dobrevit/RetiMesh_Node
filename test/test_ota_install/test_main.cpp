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


// Installing an update. Every test here asks the same question in a different
// way: did anything switch the boot slot that should not have? A node that
// refuses a bad update is working; a node that installs one is unreachable.
#include <unity.h>
#include <map>
#include <string>
#include <vector>
#include <SHA256.h>
#include "../../src/sys/OtaInstaller.h"

namespace FM = FirmwareManifest;

// --- a flash target that can be made to fail the way real ones do -----------

struct FakeTarget {
  std::vector<uint8_t> slot;
  std::string          log;
  bool hasSlot = true;
  uint32_t size = 1966080;
  bool failBegin = false, failWrite = false, failFinish = false;
  bool failReadBack = false, failSwitch = false;
  bool switched = false;
  // Flash that takes a write and stores something else — the case hashing the
  // input instead of the partition would miss entirely.
  bool corruptOnWrite = false;

  bool haveSlot() const { return hasSlot; }
  uint32_t slotSize() const { return size; }
  uint32_t slotId() const { return 0x1F0000; }
  bool begin(size_t) { log += "begin;"; slot.clear(); return !failBegin; }
  bool write(const uint8_t* d, size_t n) {
    log += "write;";
    if (failWrite) return false;
    for (size_t i = 0; i < n; i++) slot.push_back(corruptOnWrite ? (uint8_t)(d[i] ^ 0xFF) : d[i]);
    return true;
  }
  bool finish() { log += "finish;"; return !failFinish; }
  bool readBack(size_t off, uint8_t* buf, size_t n) {
    if (failReadBack || off + n > slot.size()) return false;
    memcpy(buf, slot.data() + off, n);
    return true;
  }
  bool switchTo() { log += "switch;"; if (failSwitch) return false; switched = true; return true; }
};

struct FakeStore {
  std::map<std::string, uint32_t> values;
  bool has(const char* k) const { return values.count(k) != 0; }
  uint32_t get(const char* k, uint32_t f) const {
    auto it = values.find(k); return it == values.end() ? f : it->second;
  }
  void put(const char* k, uint32_t v) { values[k] = v; }
  void drop(const char* k) { values.erase(k); }
};

struct MemorySource : Ota::Source {
  const uint8_t* data; size_t len; size_t pos = 0; size_t chunk;
  MemorySource(const uint8_t* d, size_t l, size_t c = 300) : data(d), len(l), chunk(c) {}
  size_t read(uint8_t* buf, size_t max) override {
    size_t n = len - pos; if (n > max) n = max; if (n > chunk) n = chunk;
    memcpy(buf, data + pos, n); pos += n; return n;
  }
};

// --- a manifest and an image that go together ------------------------------
//
// The signature is faked here on purpose: what these tests exercise is the
// install sequence, and test_firmware_manifest already checks the same
// FM::check() against signatures the signing tool really made. Wiring real keys
// in here would test Ed25519 a second time and the ordering not at all.

static uint8_t IMAGE[2048];
static uint8_t DELEGATE[FM::KEY_SIZE];

static bool fakeVerify(const uint8_t* sig, const uint8_t* key, const uint8_t*, size_t len) {
  return sig[0] == key[0] && sig[1] == (uint8_t)len;
}
static void fakeSign(uint8_t* sig, const uint8_t* key, size_t len) {
  memset(sig, 0, FM::SIGNATURE_SIZE); sig[0] = key[0]; sig[1] = (uint8_t)len;
}
static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void buildManifest(uint8_t* b, const uint8_t* image, size_t imageLen,
                          uint32_t secureVersion = 3) {
  memset(b, 0, FM::SIZE);
  memcpy(b, FM::MAGIC, 4);
  b[4] = FM::FORMAT_VERSION;
  SHA256 sha; sha.update(image, imageLen); sha.finalize(b + 8, FM::HASH_SIZE);
  put32(b + 40, (uint32_t)imageLen);
  put32(b + 44, secureVersion);
  strncpy((char*)b + 48, OTA_BOARD_ID, FM::BOARD_LEN);
  strncpy((char*)b + 64, "v1.2.3", FM::VERSION_LEN);
  put32(b + 96, 1966080);
  memcpy(b + FM::DELEGATION_RECORD_OFFSET, DELEGATE, FM::KEY_SIZE);
  put32(b + FM::DELEGATION_RECORD_OFFSET + 32, FM::PURPOSE_FIRMWARE);
  put32(b + FM::DELEGATION_RECORD_OFFSET + 36, 0);
  fakeSign(b + FM::ROOT_SIG_OFFSET, Ota::roots()[0], FM::DELEGATION_RECORD_SIZE);
  fakeSign(b + FM::DELEGATE_SIG_OFFSET, DELEGATE, FM::IMAGE_RECORD_SIZE);
}

// The real installer, handed the fake verifier. Nothing here reimplements the
// sequence: these tests drive Ota::Installer itself.
namespace {
using Floor = Ota::Floor<FakeStore>;

struct Harness {
  FakeTarget target;
  FakeStore  store;
  Floor      floor{store};
  Ota::Installer<FakeTarget, FakeStore> installer{target, floor, fakeVerify};
  uint8_t    manifest[FM::SIZE];

  Ota::Outcome run(size_t imageLen = sizeof(IMAGE), size_t chunk = 300) {
    MemorySource src(IMAGE, imageLen, chunk);
    return installer.install(manifest, FM::SIZE, src);
  }
};
}  // namespace

// --- tests -----------------------------------------------------------------

static void test_a_good_image_is_written_hashed_and_switched_to() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  const Ota::Outcome out = h.run();
  TEST_ASSERT_EQUAL((int)Ota::Install::Ok, (int)out.result);
  TEST_ASSERT_TRUE(h.target.switched);
  TEST_ASSERT_EQUAL_UINT32(sizeof(IMAGE), (uint32_t)out.bytesWritten);
  TEST_ASSERT_EQUAL_MEMORY(IMAGE, h.target.slot.data(), sizeof(IMAGE));
}

static void test_the_version_is_staged_only_after_the_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE), 42);
  h.run();
  TEST_ASSERT_TRUE(h.floor.haveStaged());
  TEST_ASSERT_EQUAL_UINT32(42, h.store.get("ota_staged", 0));
  TEST_ASSERT_EQUAL_UINT32(0x1F0000, h.store.get("ota_slot", 0));
  // and the floor itself has not moved: that is the next boot's job
  TEST_ASSERT_EQUAL_UINT32(0, h.floor.accepted());
}

static void test_a_refused_manifest_never_touches_the_slot() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.manifest[FM::ROOT_SIG_OFFSET] ^= 0xFF;          // not signed by a known root
  const Ota::Outcome out = h.run();
  TEST_ASSERT_EQUAL((int)Ota::Install::Refused, (int)out.result);
  TEST_ASSERT_EQUAL((int)FM::Result::UnknownRoot, (int)out.manifestResult);
  TEST_ASSERT_EQUAL_STRING("", h.target.log.c_str());   // begin() was never called
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_an_image_that_stops_early_does_not_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  const Ota::Outcome out = h.run(sizeof(IMAGE) - 64);   // source runs dry
  TEST_ASSERT_EQUAL((int)Ota::Install::ShortImage, (int)out.result);
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_an_image_longer_than_it_declared_does_not_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE) - 128);
  const Ota::Outcome out = h.run(sizeof(IMAGE));
  TEST_ASSERT_EQUAL((int)Ota::Install::LongImage, (int)out.result);
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_flash_that_stored_something_else_is_caught() {
  // The reason step 3 reads the partition back instead of hashing the input.
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.corruptOnWrite = true;
  const Ota::Outcome out = h.run();
  TEST_ASSERT_EQUAL((int)Ota::Install::HashMismatch, (int)out.result);
  TEST_ASSERT_FALSE(h.target.switched);
  TEST_ASSERT_FALSE(h.floor.haveStaged());
}

static void test_a_write_failure_does_not_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.failWrite = true;
  TEST_ASSERT_EQUAL((int)Ota::Install::WriteFailed, (int)h.run().result);
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_an_unbootable_image_does_not_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.failFinish = true;
  TEST_ASSERT_EQUAL((int)Ota::Install::FinishFailed, (int)h.run().result);
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_a_slot_that_cannot_be_read_back_does_not_switch() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.failReadBack = true;
  TEST_ASSERT_EQUAL((int)Ota::Install::ReadBackFailed, (int)h.run().result);
  TEST_ASSERT_FALSE(h.target.switched);
}

static void test_a_failed_switch_stages_nothing() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.failSwitch = true;
  TEST_ASSERT_EQUAL((int)Ota::Install::SwitchFailed, (int)h.run().result);
  TEST_ASSERT_FALSE(h.floor.haveStaged());
}

static void test_a_board_with_one_app_slot_says_so() {
  Harness h;
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
  h.target.hasSlot = false;
  TEST_ASSERT_EQUAL((int)Ota::Install::NoSlot, (int)h.run().result);
}

static void test_an_image_below_the_floor_is_refused_before_writing() {
  Harness h;
  h.store.put("ota_floor", 10);
  buildManifest(h.manifest, IMAGE, sizeof(IMAGE), 4);
  const Ota::Outcome out = h.run();
  TEST_ASSERT_EQUAL((int)Ota::Install::Refused, (int)out.result);
  TEST_ASSERT_EQUAL((int)FM::Result::Rollback, (int)out.manifestResult);
  TEST_ASSERT_EQUAL_STRING("", h.target.log.c_str());
}

static void test_the_source_may_hand_over_any_chunk_size() {
  for (size_t chunk : {(size_t)1, (size_t)7, (size_t)1024, (size_t)4096}) {
    Harness h;
    buildManifest(h.manifest, IMAGE, sizeof(IMAGE));
    const Ota::Outcome out = h.run(sizeof(IMAGE), chunk);
    TEST_ASSERT_EQUAL((int)Ota::Install::Ok, (int)out.result);
    TEST_ASSERT_EQUAL_MEMORY(IMAGE, h.target.slot.data(), sizeof(IMAGE));
  }
}

static void test_every_refusal_can_be_explained() {
  const Ota::Install all[] = {
    Ota::Install::Ok, Ota::Install::NoSlot, Ota::Install::Refused,
    Ota::Install::ShortImage, Ota::Install::LongImage, Ota::Install::WriteFailed,
    Ota::Install::FinishFailed, Ota::Install::ReadBackFailed,
    Ota::Install::HashMismatch, Ota::Install::SwitchFailed,
  };
  for (Ota::Install r : all) {
    TEST_ASSERT_NOT_NULL(Ota::describe(r));
    TEST_ASSERT_TRUE(strlen(Ota::describe(r)) > 1);
    TEST_ASSERT_NOT_EQUAL(0, strcmp("unknown", Ota::describe(r)));
  }
}

void setUp() {}
void tearDown() {}

int main() {
  for (size_t i = 0; i < sizeof(IMAGE); i++) IMAGE[i] = (uint8_t)(i * 31 + 7);
  memset(DELEGATE, 0xC3, sizeof(DELEGATE));

  UNITY_BEGIN();
  RUN_TEST(test_a_good_image_is_written_hashed_and_switched_to);
  RUN_TEST(test_the_version_is_staged_only_after_the_switch);
  RUN_TEST(test_a_refused_manifest_never_touches_the_slot);
  RUN_TEST(test_an_image_that_stops_early_does_not_switch);
  RUN_TEST(test_an_image_longer_than_it_declared_does_not_switch);
  RUN_TEST(test_flash_that_stored_something_else_is_caught);
  RUN_TEST(test_a_write_failure_does_not_switch);
  RUN_TEST(test_an_unbootable_image_does_not_switch);
  RUN_TEST(test_a_slot_that_cannot_be_read_back_does_not_switch);
  RUN_TEST(test_a_failed_switch_stages_nothing);
  RUN_TEST(test_a_board_with_one_app_slot_says_so);
  RUN_TEST(test_an_image_below_the_floor_is_refused_before_writing);
  RUN_TEST(test_the_source_may_hand_over_any_chunk_size);
  RUN_TEST(test_every_refusal_can_be_explained);
  return UNITY_END();
}
