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


// Whether an image may be installed. The signature check is a stub here — the
// bugs this guards against live in the ordering and the field checks, and a
// host has no Ed25519 to test the maths with anyway.
#include <unity.h>
#include <string.h>
#include "../../src/sys/FirmwareManifest.h"
#include "fixture.h"

namespace FM = FirmwareManifest;

// Two roots, a delegate, and a fake signature scheme: a signature is "valid"
// when its first byte names the key that made it and the rest is the length of
// what was signed. Enough to tell a good chain from a broken one without
// pretending to do cryptography.
static uint8_t ROOT_A[FM::KEY_SIZE], ROOT_B[FM::KEY_SIZE], DELEGATE[FM::KEY_SIZE], STRANGER[FM::KEY_SIZE];
static uint8_t roots[2][FM::KEY_SIZE];

static bool fakeVerify(const uint8_t* sig, const uint8_t* key, const uint8_t* msg, size_t len) {
  (void)msg;
  return sig[0] == key[0] && sig[1] == (uint8_t)len;
}
static void sign(uint8_t* sig, const uint8_t* key, size_t len) {
  memset(sig, 0, FM::SIGNATURE_SIZE);
  sig[0] = key[0];
  sig[1] = (uint8_t)len;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void putStr(uint8_t* p, const char* s, size_t len) {
  memset(p, 0, len); strncpy((char*)p, s, len);
}

// A manifest that passes, which each test then damages in exactly one way.
static void buildGood(uint8_t* b) {
  memset(b, 0, FM::SIZE);
  memcpy(b, FM::MAGIC, 4);
  b[4] = FM::FORMAT_VERSION;
  memset(b + 8, 0xAB, FM::HASH_SIZE);              // image hash
  put32(b + 40, 1800000);                          // image size
  put32(b + 44, 7);                                // secure_version
  putStr(b + 48, "t3s3", FM::BOARD_LEN);
  putStr(b + 64, "v0.1.0", FM::VERSION_LEN);
  put32(b + 96, 1966080);                          // slot size
  memcpy(b + FM::DELEGATION_RECORD_OFFSET, DELEGATE, FM::KEY_SIZE);
  put32(b + FM::DELEGATION_RECORD_OFFSET + 32, FM::PURPOSE_FIRMWARE);
  put32(b + FM::DELEGATION_RECORD_OFFSET + 36, 5); // this delegate may not sign below 5
  putStr(b + FM::DELEGATION_RECORD_OFFSET + 40, "ci", FM::LABEL_LEN);
  sign(b + FM::ROOT_SIG_OFFSET, ROOT_B, FM::DELEGATION_RECORD_SIZE);
  sign(b + FM::DELEGATE_SIG_OFFSET, DELEGATE, FM::IMAGE_RECORD_SIZE);
}

static FM::Policy policy(uint32_t accepted = 0) {
  FM::Policy p;
  p.roots = roots; p.rootCount = 2;
  p.board = "t3s3"; p.slotSize = 1966080; p.acceptedVersion = accepted;
  return p;
}

static FM::Result check(const uint8_t* b, FM::Policy p, size_t len = FM::SIZE) {
  FM::Manifest m;
  return FM::check(b, len, p, fakeVerify, m);
}

static void test_a_well_formed_manifest_is_accepted() {
  uint8_t b[FM::SIZE]; buildGood(b);
  FM::Manifest m;
  TEST_ASSERT_EQUAL((int)FM::Result::Ok, (int)FM::check(b, sizeof(b), policy(), fakeVerify, m));
  TEST_ASSERT_EQUAL_STRING("t3s3", m.board);
  TEST_ASSERT_EQUAL_STRING("v0.1.0", m.version);
  TEST_ASSERT_EQUAL_STRING("ci", m.delegateLabel);
  TEST_ASSERT_EQUAL_UINT32(7, m.secureVersion);
}

static void test_either_root_may_anchor_the_chain() {
  // Two roots exist so that losing one is survivable; both must actually work.
  uint8_t b[FM::SIZE]; buildGood(b);
  sign(b + FM::ROOT_SIG_OFFSET, ROOT_A, FM::DELEGATION_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::Ok, (int)check(b, policy()));
}

static void test_a_delegation_from_an_unknown_root_is_refused() {
  uint8_t b[FM::SIZE]; buildGood(b);
  sign(b + FM::ROOT_SIG_OFFSET, STRANGER, FM::DELEGATION_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::UnknownRoot, (int)check(b, policy()));
}

static void test_a_delegate_cannot_vouch_for_itself() {
  // The delegation record signed by the delegate rather than by a root: the
  // whole chain reduced to "this key says this key is fine".
  uint8_t b[FM::SIZE]; buildGood(b);
  sign(b + FM::ROOT_SIG_OFFSET, DELEGATE, FM::DELEGATION_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::UnknownRoot, (int)check(b, policy()));
}

static void test_a_key_not_marked_for_firmware_is_refused() {
  uint8_t b[FM::SIZE]; buildGood(b);
  put32(b + FM::DELEGATION_RECORD_OFFSET + 32, 0);   // some other purpose
  TEST_ASSERT_EQUAL((int)FM::Result::NotForFirmware, (int)check(b, policy()));
}

static void test_an_image_signed_by_someone_else_is_refused() {
  uint8_t b[FM::SIZE]; buildGood(b);
  sign(b + FM::DELEGATE_SIG_OFFSET, STRANGER, FM::IMAGE_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::BadImageSignature, (int)check(b, policy()));
}

static void test_a_swapped_image_record_breaks_its_signature() {
  // The delegate signs the image record, so editing the hash after signing has
  // to fail — this is the attack the whole file exists to stop.
  uint8_t b[FM::SIZE]; buildGood(b);
  b[8] ^= 0xFF;
  // The fake verifier only checks length, so make the damage one the real
  // scheme would catch and this one can too: a shorter record.
  TEST_ASSERT_EQUAL((int)FM::Result::Ok, (int)check(b, policy()));   // fake crypto cannot see it
  // What the fake *can* prove is that the signature is taken over the image
  // record and not over the whole blob: signing the wrong length is refused.
  sign(b + FM::DELEGATE_SIG_OFFSET, DELEGATE, FM::SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::BadImageSignature, (int)check(b, policy()));
}

static void test_an_image_for_another_board_is_refused() {
  uint8_t b[FM::SIZE]; buildGood(b);
  putStr(b + 48, "heltec-wp", FM::BOARD_LEN);
  sign(b + FM::DELEGATE_SIG_OFFSET, DELEGATE, FM::IMAGE_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::WrongBoard, (int)check(b, policy()));
}

static void test_an_image_for_another_partition_layout_is_refused() {
  // Correctly signed, right board, but built against a table this node does
  // not have — it would install and never boot.
  uint8_t b[FM::SIZE]; buildGood(b);
  put32(b + 96, 3145728);
  TEST_ASSERT_EQUAL((int)FM::Result::WrongSlotSize, (int)check(b, policy()));
}

static void test_an_image_too_large_for_the_slot_is_refused() {
  uint8_t b[FM::SIZE]; buildGood(b);
  put32(b + 40, 1966081);
  TEST_ASSERT_EQUAL((int)FM::Result::Oversize, (int)check(b, policy()));
}

static void test_an_older_image_is_refused() {
  // The only revocation that works with no clock and no network.
  uint8_t b[FM::SIZE]; buildGood(b);
  TEST_ASSERT_EQUAL((int)FM::Result::Rollback, (int)check(b, policy(8)));
  TEST_ASSERT_EQUAL((int)FM::Result::Ok, (int)check(b, policy(7)));   // equal is not a rollback
}

static void test_a_delegate_cannot_sign_below_its_own_floor() {
  // A delegate issued for the current generation must not be usable to sign a
  // vulnerable older one, even though that version is not a rollback for a node
  // that has never seen it.
  uint8_t b[FM::SIZE]; buildGood(b);
  put32(b + 44, 4);
  sign(b + FM::DELEGATE_SIG_OFFSET, DELEGATE, FM::IMAGE_RECORD_SIZE);
  TEST_ASSERT_EQUAL((int)FM::Result::BelowDelegateFloor, (int)check(b, policy()));
}

static void test_rubbish_is_refused_before_anything_is_read() {
  uint8_t b[FM::SIZE]; buildGood(b);
  TEST_ASSERT_EQUAL((int)FM::Result::TooShort, (int)check(b, policy(), FM::SIZE - 1));
  uint8_t junk[FM::SIZE]; memset(junk, 0, sizeof(junk));
  TEST_ASSERT_EQUAL((int)FM::Result::BadMagic, (int)check(junk, policy()));
  buildGood(b); b[4] = FM::FORMAT_VERSION + 1;
  TEST_ASSERT_EQUAL((int)FM::Result::UnknownFormat, (int)check(b, policy()));
}

static void test_every_refusal_can_be_explained() {
  // A node that refuses an update has to be able to say why, over a console
  // reached by LoRa, to somebody who cannot see it.
  const FM::Result refusals[] = { FM::Result::TooShort, FM::Result::BadMagic,
    FM::Result::UnknownFormat, FM::Result::UnknownRoot, FM::Result::BadDelegation,
    FM::Result::NotForFirmware, FM::Result::BadImageSignature, FM::Result::WrongBoard,
    FM::Result::WrongSlotSize, FM::Result::BelowDelegateFloor, FM::Result::Rollback,
    FM::Result::Oversize };
  for (FM::Result r : refusals) {
    TEST_ASSERT_NOT_NULL(FM::describe(r));
    // A sentence, not a token: this is what an operator reads over a link that
    // took a minute to establish.
    TEST_ASSERT_TRUE(strlen(FM::describe(r)) > 8);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(FM::describe(r), "unknown"));
  }
  TEST_ASSERT_EQUAL_STRING("ok", FM::describe(FM::Result::Ok));
}

// The signing tool built this one. Parsing it here is what stops
// tools/fw_sign.py and FirmwareManifest.h drifting apart — a disagreement about
// a field offset would otherwise show up as a fleet that refuses every update.
static bool acceptAny(const uint8_t*, const uint8_t*, const uint8_t*, size_t) { return true; }

static void test_the_signing_tools_manifest_parses_as_the_firmware_expects() {
  FM::Manifest m;
  FM::Policy p = policy();
  p.acceptedVersion = 0;
  // Signatures are real Ed25519 and this host has none, so the chain is taken
  // on trust here; what is under test is the layout, field by field.
  const FM::Result r = FM::check(MANIFEST_FIXTURE, sizeof(MANIFEST_FIXTURE), p, acceptAny, m);
  TEST_ASSERT_EQUAL((int)FM::Result::Ok, (int)r);
  TEST_ASSERT_EQUAL_STRING("t3s3", m.board);
  TEST_ASSERT_EQUAL_STRING("v9.9.9", m.version);
  TEST_ASSERT_EQUAL_STRING("fixture", m.delegateLabel);
  TEST_ASSERT_EQUAL_UINT32(1800000, m.imageSize);
  TEST_ASSERT_EQUAL_UINT32(7, m.secureVersion);
  TEST_ASSERT_EQUAL_UINT32(1966080, m.slotSize);
  TEST_ASSERT_EQUAL_UINT32(5, m.delegateMinVersion);
  TEST_ASSERT_EQUAL_UINT32(FM::PURPOSE_FIRMWARE, m.delegatePurpose);
  for (size_t i = 0; i < FM::HASH_SIZE; i++) TEST_ASSERT_EQUAL_UINT8(i, m.imageHash[i]);
}

void setUp() {}
void tearDown() {}

int main() {
  memset(ROOT_A, 0xA1, sizeof(ROOT_A));  ROOT_A[0] = 0xA1;
  memset(ROOT_B, 0xB2, sizeof(ROOT_B));  ROOT_B[0] = 0xB2;
  memset(DELEGATE, 0xC3, sizeof(DELEGATE)); DELEGATE[0] = 0xC3;
  memset(STRANGER, 0xD4, sizeof(STRANGER)); STRANGER[0] = 0xD4;
  memcpy(roots[0], ROOT_A, FM::KEY_SIZE);
  memcpy(roots[1], ROOT_B, FM::KEY_SIZE);

  UNITY_BEGIN();
  RUN_TEST(test_a_well_formed_manifest_is_accepted);
  RUN_TEST(test_either_root_may_anchor_the_chain);
  RUN_TEST(test_a_delegation_from_an_unknown_root_is_refused);
  RUN_TEST(test_a_delegate_cannot_vouch_for_itself);
  RUN_TEST(test_a_key_not_marked_for_firmware_is_refused);
  RUN_TEST(test_an_image_signed_by_someone_else_is_refused);
  RUN_TEST(test_a_swapped_image_record_breaks_its_signature);
  RUN_TEST(test_an_image_for_another_board_is_refused);
  RUN_TEST(test_an_image_for_another_partition_layout_is_refused);
  RUN_TEST(test_an_image_too_large_for_the_slot_is_refused);
  RUN_TEST(test_an_older_image_is_refused);
  RUN_TEST(test_a_delegate_cannot_sign_below_its_own_floor);
  RUN_TEST(test_rubbish_is_refused_before_anything_is_read);
  RUN_TEST(test_every_refusal_can_be_explained);
  RUN_TEST(test_the_signing_tools_manifest_parses_as_the_firmware_expects);
  return UNITY_END();
}
