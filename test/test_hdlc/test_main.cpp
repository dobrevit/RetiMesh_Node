// SPDX-License-Identifier: GPL-3.0-or-later
// Host-native tests for the RNS TCP framing (HDLC.h).
#include <unity.h>
#include <vector>
#include "HDLC.h"

static std::vector<std::vector<uint8_t>> deframeAll(const uint8_t* stream, size_t len) {
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  for (size_t i = 0; i < len; i++)
    d.feed(stream[i], [&](const uint8_t* p, size_t n) { out.emplace_back(p, p + n); });
  return out;
}

void test_frame_escapes_flag_and_esc() {
  const uint8_t in[] = { 0x01, 0x7E, 0x02, 0x7D, 0x03 };
  uint8_t out[32];
  size_t n = HDLC::frame(in, sizeof(in), out, sizeof(out));
  const uint8_t expect[] = { 0x7E, 0x01, 0x7D, 0x5E, 0x02, 0x7D, 0x5D, 0x03, 0x7E };
  TEST_ASSERT_EQUAL_UINT(sizeof(expect), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, out, n);
}

void test_frame_rejects_small_buffer() {
  const uint8_t in[] = { 0x7E, 0x7E };
  uint8_t out[4];
  TEST_ASSERT_EQUAL_UINT(0, HDLC::frame(in, sizeof(in), out, sizeof(out)));   // needs 2 + 4
}

void test_roundtrip_two_packets_and_garbage() {
  const uint8_t a[] = { 0x10, 0x7E, 0x7D, 0x20 };
  const uint8_t b[] = { 0xAA };
  uint8_t stream[64]; size_t len = 0;
  stream[len++] = 0x55;                                      // garbage before the first flag
  len += HDLC::frame(a, sizeof(a), stream + len, sizeof(stream) - len);
  stream[len++] = 0x7E; stream[len++] = 0x7E;                // empty frames are ignored
  len += HDLC::frame(b, sizeof(b), stream + len, sizeof(stream) - len);
  auto pkts = deframeAll(stream, len);
  TEST_ASSERT_EQUAL_UINT(2, pkts.size());
  TEST_ASSERT_EQUAL_UINT(sizeof(a), pkts[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, pkts[0].data(), sizeof(a));
  TEST_ASSERT_EQUAL_UINT(1, pkts[1].size());
  TEST_ASSERT_EQUAL_UINT8(0xAA, pkts[1][0]);
}

void test_oversized_frame_is_dropped_and_parser_recovers() {
  std::vector<uint8_t> stream;
  stream.push_back(0x7E);
  for (int i = 0; i < RNS_MTU + 10; i++) stream.push_back(0x01);   // > MTU payload
  stream.push_back(0x7E);
  const uint8_t ok[] = { 0x42, 0x43 };
  uint8_t buf[16]; size_t n = HDLC::frame(ok, sizeof(ok), buf, sizeof(buf));
  stream.insert(stream.end(), buf, buf + n);
  auto pkts = deframeAll(stream.data(), stream.size());
  TEST_ASSERT_EQUAL_UINT(1, pkts.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(ok, pkts[0].data(), sizeof(ok));
}

void test_mtu_sized_frame_passes() {
  std::vector<uint8_t> payload(RNS_MTU, 0x7E);              // worst case: everything escapes
  std::vector<uint8_t> buf(HDLC::frameCapacity(RNS_MTU));
  size_t n = HDLC::frame(payload.data(), payload.size(), buf.data(), buf.size());
  TEST_ASSERT_EQUAL_UINT(2 + 2 * RNS_MTU, n);
  auto pkts = deframeAll(buf.data(), n);
  TEST_ASSERT_EQUAL_UINT(1, pkts.size());
  TEST_ASSERT_EQUAL_UINT(RNS_MTU, pkts[0].size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_frame_escapes_flag_and_esc);
  RUN_TEST(test_frame_rejects_small_buffer);
  RUN_TEST(test_roundtrip_two_packets_and_garbage);
  RUN_TEST(test_oversized_frame_is_dropped_and_parser_recovers);
  RUN_TEST(test_mtu_sized_frame_passes);
  return UNITY_END();
}
