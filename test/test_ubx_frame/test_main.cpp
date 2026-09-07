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


// The one binary message this firmware sends a GNSS receiver, held to the
// bytes the specification asks for.
//
// This is worth a suite because nothing on a bench will tell you it is wrong.
// A UBX frame with a bad checksum is not rejected with an error — the receiver
// silently discards it, and the symptom is a module that goes on drawing
// twenty milliamps exactly as if no message had been sent, which is also the
// symptom of a module that does not support the message, of a TX line wired to
// the wrong pin, and of a receiver that is not u-blox at all. Four
// indistinguishable failures, one of which is arithmetic; this suite removes
// that one from the list.
//
// Everything asserted here comes from the u-blox M10 SPG 5.10 Interface
// description, UBX-21035062 R03 — §3.2 (frame structure), §3.4 (checksum) and
// §3.16.6 (UBX-RXM-PMREQ). UbxFrame.h quotes the relevant lines.
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "../../src/sys/UbxFrame.h"

// §3.4's pseudocode, transcribed here rather than called from the header: the
// point of a reference implementation is that it is a second one.
//
//   CK_A = 0, CK_B = 0
//   For (I = 0; I < N; I++) { CK_A = CK_A + Buffer[I]; CK_B = CK_B + CK_A }
//
// with both held to eight bits.
static void referenceChecksum(const uint8_t* buf, size_t n, uint8_t& a, uint8_t& b) {
  unsigned ckA = 0, ckB = 0;
  for (size_t i = 0; i < n; i++) {
    ckA = (ckA + buf[i]) & 0xFF;
    ckB = (ckB + ckA) & 0xFF;
  }
  a = (uint8_t)ckA;
  b = (uint8_t)ckB;
}

// ---------------------------------------------------------------------------
// The checksum

// The specification's own worked description has no numeric vector in it, so
// the arithmetic is pinned two ways: against the transcribed pseudocode above,
// and — for the message actually sent — against a frame computed outside this
// program entirely (see the literal further down).
static void test_the_checksum_matches_the_written_algorithm() {
  const uint8_t body[] = { 0x02, 0x41, 0x10, 0x00, 0x00, 0xFF, 0x7F, 0x80, 0x01 };
  for (size_t n = 0; n <= sizeof(body); n++) {
    uint8_t a = 0xAA, b = 0x55, ra = 0, rb = 0;
    Ubx::checksum(body, n, a, b);
    referenceChecksum(body, n, ra, rb);
    TEST_ASSERT_EQUAL_UINT8(ra, a);
    TEST_ASSERT_EQUAL_UINT8(rb, b);
  }
}

// Zero bytes is zero, and both accumulators are written rather than left as
// they were found. A checksum routine that returns its inputs untouched on an
// empty buffer would pass every other test here.
static void test_an_empty_buffer_zeroes_both_bytes() {
  uint8_t a = 0xAA, b = 0x55;
  Ubx::checksum(nullptr, 0, a, b);
  TEST_ASSERT_EQUAL_UINT8(0, a);
  TEST_ASSERT_EQUAL_UINT8(0, b);
}

// The eight-bit mask is not optional: the specification says so in as many
// words. A 32-bit accumulator that is never masked agrees with a masked one
// until the sum passes 255, which a real payload does within a few bytes.
static void test_both_bytes_wrap_at_eight_bits() {
  uint8_t body[8];
  memset(body, 0xFF, sizeof(body));
  uint8_t a = 0, b = 0;
  Ubx::checksum(body, sizeof(body), a, b);
  // CK_A is 8*255 mod 256 = 248; CK_B is the sum of the running CK_A values,
  // 255+254+...+248 = 2012 mod 256 = 220.
  TEST_ASSERT_EQUAL_UINT8(248, a);
  TEST_ASSERT_EQUAL_UINT8(220, b);
}

// Fletcher is order-sensitive, which is the whole reason it is used instead of
// a plain sum: two bytes swapped anywhere in the message must change CK_B.
// A transposed length field or a byte-swapped duration is precisely the
// mistake this catches.
static void test_swapping_two_bytes_changes_the_checksum() {
  uint8_t body[] = { 0x02, 0x41, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04 };
  uint8_t a1 = 0, b1 = 0, a2 = 0, b2 = 0;
  Ubx::checksum(body, sizeof(body), a1, b1);
  const uint8_t t = body[4]; body[4] = body[7]; body[7] = t;
  Ubx::checksum(body, sizeof(body), a2, b2);
  TEST_ASSERT_EQUAL_UINT8(a1, a2);          // the plain sum is blind to it
  TEST_ASSERT_NOT_EQUAL_UINT8(b1, b2);      // and CK_B is not
}

// Any single byte altered anywhere is caught. Exhaustive over position and
// over every one of the 255 wrong values, because the frame is short enough
// that "probably" is not needed.
static void test_every_single_byte_error_is_caught() {
  const uint8_t body[] = { 0x02, 0x41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                           0x08, 0x00, 0x00, 0x00 };
  uint8_t a0 = 0, b0 = 0;
  Ubx::checksum(body, sizeof(body), a0, b0);
  for (size_t i = 0; i < sizeof(body); i++) {
    uint8_t bad[sizeof(body)];
    memcpy(bad, body, sizeof(body));
    for (unsigned v = 0; v < 256; v++) {
      if ((uint8_t)v == body[i]) continue;
      bad[i] = (uint8_t)v;
      uint8_t a = 0, b = 0;
      Ubx::checksum(bad, sizeof(bad), a, b);
      TEST_ASSERT_TRUE(a != a0 || b != b0);
    }
  }
}

// ---------------------------------------------------------------------------
// The frame

// §3.2, field by field: preamble, class, id, then the length as an unsigned
// little-endian 16-bit integer covering the payload only.
static void test_the_frame_is_laid_out_as_the_specification_says() {
  const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x11 };
  uint8_t out[32];
  memset(out, 0xCC, sizeof(out));
  const size_t n = Ubx::frame(0x06, 0x04, payload, sizeof(payload), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(Ubx::kOverhead + sizeof(payload), n);
  TEST_ASSERT_EQUAL_UINT8(0xB5, out[0]);
  TEST_ASSERT_EQUAL_UINT8(0x62, out[1]);
  TEST_ASSERT_EQUAL_UINT8(0x06, out[2]);
  TEST_ASSERT_EQUAL_UINT8(0x04, out[3]);
  TEST_ASSERT_EQUAL_UINT8(5, out[4]);       // low byte first
  TEST_ASSERT_EQUAL_UINT8(0, out[5]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out + 6, sizeof(payload));
  // And the checksum covers class..payload — not the preamble, not itself.
  uint8_t a = 0, b = 0;
  referenceChecksum(out + 2, 4 + sizeof(payload), a, b);
  TEST_ASSERT_EQUAL_UINT8(a, out[n - 2]);
  TEST_ASSERT_EQUAL_UINT8(b, out[n - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCC, out[n]);    // nothing written past the frame
}

// The length field is two bytes and a payload can cross 255. A build that
// wrote it as one byte, or the wrong way round, passes every short-payload
// test there is.
static void test_a_long_payload_fills_both_length_bytes() {
  uint8_t payload[300];
  memset(payload, 0x5A, sizeof(payload));
  uint8_t out[sizeof(payload) + Ubx::kOverhead];
  const size_t n = Ubx::frame(0x02, 0x41, payload, (uint16_t)sizeof(payload), out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(out), n);
  TEST_ASSERT_EQUAL_UINT8(300 & 0xFF, out[4]);
  TEST_ASSERT_EQUAL_UINT8(300 >> 8, out[5]);
}

// A buffer that cannot hold the whole frame gets nothing at all. Half a frame
// on a UART is worse than no frame: the receiver is left hunting for a
// preamble inside the payload of the next thing it is sent.
static void test_a_short_buffer_writes_nothing() {
  const uint8_t payload[] = { 1, 2, 3 };
  uint8_t out[Ubx::kOverhead + sizeof(payload) - 1];
  memset(out, 0xCC, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, Ubx::frame(0x02, 0x41, payload, sizeof(payload), out, sizeof(out)));
  for (size_t i = 0; i < sizeof(out); i++) TEST_ASSERT_EQUAL_UINT8(0xCC, out[i]);
  TEST_ASSERT_EQUAL_size_t(0, Ubx::frame(0x02, 0x41, payload, sizeof(payload), nullptr, 99));
  // A null payload is only legal with nothing to copy from it.
  uint8_t ok[Ubx::kOverhead];
  TEST_ASSERT_EQUAL_size_t(0, Ubx::frame(0x02, 0x41, nullptr, 4, ok, sizeof(ok)));
  TEST_ASSERT_EQUAL_size_t(Ubx::kOverhead, Ubx::frame(0x02, 0x41, nullptr, 0, ok, sizeof(ok)));
}

// ---------------------------------------------------------------------------
// UBX-RXM-PMREQ

// §3.16.6's constants, as named here. Their being wrong is the one failure
// that looks exactly like a receiver that does not support the message, so
// they are asserted rather than trusted — and the bit numbers in particular:
// uartrx is bit 3 and extint0 is bit 5, which is neither consecutive nor the
// order the table lists them in. Against the M10 interface description this
// firmware was written from, and against nothing else: the M8 tables number
// them the same way, so a test that claimed a difference would be teaching a
// future reader something untrue.
static void test_the_message_identifiers_and_bits_are_the_m10_ones() {
  TEST_ASSERT_EQUAL_UINT8(0x02, Ubx::kClassRxm);
  TEST_ASSERT_EQUAL_UINT8(0x41, Ubx::kIdPmreq);
  TEST_ASSERT_EQUAL_size_t(16, Ubx::kPmreqLen);
  TEST_ASSERT_EQUAL_size_t(24, Ubx::kPmreqFrame);
  TEST_ASSERT_EQUAL_UINT32(1u << 1, Ubx::kPmreqBackup);
  TEST_ASSERT_EQUAL_UINT32(1u << 2, Ubx::kPmreqForce);
  TEST_ASSERT_EQUAL_UINT32(1u << 3, Ubx::kWakeUartRx);
  TEST_ASSERT_EQUAL_UINT32(1u << 5, Ubx::kWakeExtInt0);
  TEST_ASSERT_EQUAL_UINT32(1u << 6, Ubx::kWakeExtInt1);
  TEST_ASSERT_EQUAL_UINT32(1u << 7, Ubx::kWakeSpiCs);
}

// The frame the T-Deck's receiver is actually sent, byte for byte.
//
// This literal was produced by a separate implementation of §3.2/§3.4 outside
// this program and pasted in, so it is a genuine second opinion rather than
// this header agreeing with itself. duration 0 ("wait for a wakeup signal on a
// pin"), flags = backup, wakeupSources = uartrx.
static void test_the_backup_request_is_the_expected_twenty_four_bytes() {
  static const uint8_t kExpected[24] = {
    0xB5, 0x62,                                      // preamble
    0x02, 0x41,                                      // RXM-PMREQ
    0x10, 0x00,                                      // 16-byte payload, little-endian
    0x00, 0x00, 0x00, 0x00,                          // version 0, reserved0[3]
    0x00, 0x00, 0x00, 0x00,                          // duration 0
    0x02, 0x00, 0x00, 0x00,                          // flags: backup
    0x08, 0x00, 0x00, 0x00,                          // wakeupSources: uartrx
    0x5D, 0x4B                                       // CK_A, CK_B
  };
  uint8_t out[Ubx::kPmreqFrame];
  const size_t n = Ubx::pmreqBackup(0, Ubx::kWakeUartRx, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(kExpected), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kExpected, out, sizeof(kExpected));
}

// The same, with a duration, so the U4 is pinned little-endian in a case where
// a byte-swap would show. 30000 ms is 0x00007530, which reads 30 75 00 00.
static void test_a_duration_is_written_little_endian() {
  static const uint8_t kExpected[24] = {
    0xB5, 0x62, 0x02, 0x41, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x30, 0x75, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00,
    0x02, 0x92
  };
  uint8_t out[Ubx::kPmreqFrame];
  const size_t n = Ubx::pmreqBackup(30000, Ubx::kWakeUartRx, out, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(sizeof(kExpected), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kExpected, out, sizeof(kExpected));
}

// The force bit is never set by this builder, whatever it is asked for: it is
// documented only as "minimum power consumption", with nothing said about what
// it costs, and the wakeupSources argument must not be able to reach it.
static void test_the_force_bit_is_never_set() {
  uint8_t out[Ubx::kPmreqFrame];
  Ubx::pmreqBackup(0, 0xFFFFFFFFu, out, sizeof(out));
  const uint32_t flags = (uint32_t)out[14] | ((uint32_t)out[15] << 8) |
                         ((uint32_t)out[16] << 16) | ((uint32_t)out[17] << 24);
  TEST_ASSERT_EQUAL_UINT32(Ubx::kPmreqBackup, flags);
  TEST_ASSERT_EQUAL_UINT32(0, flags & Ubx::kPmreqForce);
}

// Reserved bytes go out as zero, which §3.3.2 requires of an input message.
static void test_the_reserved_bytes_are_zero() {
  uint8_t out[Ubx::kPmreqFrame];
  Ubx::pmreqBackup(0xFFFFFFFFu, Ubx::kWakeUartRx, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT8(0x00, out[6]);          // version
  TEST_ASSERT_EQUAL_UINT8(0x00, out[7]);          // reserved0[0]
  TEST_ASSERT_EQUAL_UINT8(0x00, out[8]);
  TEST_ASSERT_EQUAL_UINT8(0x00, out[9]);
}

// And it refuses a buffer that cannot hold it, like every other frame.
static void test_the_backup_request_needs_its_whole_buffer() {
  uint8_t out[Ubx::kPmreqFrame - 1];
  memset(out, 0xCC, sizeof(out));
  TEST_ASSERT_EQUAL_size_t(0, Ubx::pmreqBackup(0, Ubx::kWakeUartRx, out, sizeof(out)));
  for (size_t i = 0; i < sizeof(out); i++) TEST_ASSERT_EQUAL_UINT8(0xCC, out[i]);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_checksum_matches_the_written_algorithm);
  RUN_TEST(test_an_empty_buffer_zeroes_both_bytes);
  RUN_TEST(test_both_bytes_wrap_at_eight_bits);
  RUN_TEST(test_swapping_two_bytes_changes_the_checksum);
  RUN_TEST(test_every_single_byte_error_is_caught);
  RUN_TEST(test_the_frame_is_laid_out_as_the_specification_says);
  RUN_TEST(test_a_long_payload_fills_both_length_bytes);
  RUN_TEST(test_a_short_buffer_writes_nothing);
  RUN_TEST(test_the_message_identifiers_and_bits_are_the_m10_ones);
  RUN_TEST(test_the_backup_request_is_the_expected_twenty_four_bytes);
  RUN_TEST(test_a_duration_is_written_little_endian);
  RUN_TEST(test_the_force_bit_is_never_set);
  RUN_TEST(test_the_reserved_bytes_are_zero);
  RUN_TEST(test_the_backup_request_needs_its_whole_buffer);
  return UNITY_END();
}
