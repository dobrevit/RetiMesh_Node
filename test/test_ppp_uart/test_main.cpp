// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// The rule that decides who owns the bridge UART. One serial port carries
// the log, the console and PPP, and the cases that matter are the ones
// where the port must not change hands: a '~' somebody typed, a frame that
// is not a Configure-Request, a candidate the port went quiet on — and the
// ones where it must: pppd's opening frame, byte for byte, handed on intact.

#include <unity.h>
#include <vector>
#include "PppArbiter.h"

using namespace PppUart;

// pppd's first LCP Configure-Request as it leaves the wire: address/control
// FF 03, protocol C0 21, code 01, id 01, length 00 14, then options — with
// every control character escaped, as the initial ACCM requires. 0x03 and
// 0x01 become 7D 23 and 7D 21.
static const uint8_t kConfReq[] = { 0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x21, 0x7D, 0x21,
                                    0x00, 0x14, 0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x05, 0x06,
                                    0x12, 0x34, 0x56, 0x78, 0x07, 0x02, 0x08, 0x02, 0xAB, 0xCD, 0x7E };

struct Feed {
  std::vector<uint8_t> console, ppp;
  int tookOverAt = -1;
  void run(Arbiter& a, const uint8_t* bytes, size_t n, uint32_t t0 = 1000) {
    for (size_t i = 0; i < n; i++) {
      const Route r = a.feed(bytes[i], t0 + (uint32_t)i);
      if (r.sink == Sink::Console) console.insert(console.end(), r.data, r.data + r.len);
      if (r.sink == Sink::Ppp)     ppp.insert(ppp.end(), r.data, r.data + r.len);
      if (r.tookOver) tookOverAt = (int)i;
    }
  }
};

static void test_console_bytes_pass_through_one_by_one() {
  Arbiter a(true);
  Feed f;
  f.run(a, (const uint8_t*)"VERSION\n", 8);
  TEST_ASSERT_EQUAL(8, (int)f.console.size());
  TEST_ASSERT_EQUAL_MEMORY("VERSION\n", f.console.data(), 8);
  TEST_ASSERT_EQUAL(0, (int)f.ppp.size());
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
}

static void test_pppd_opening_frame_takes_the_port_and_arrives_whole() {
  Arbiter a(true);
  Feed f;
  f.run(a, kConfReq, sizeof(kConfReq));
  TEST_ASSERT_EQUAL((int)Owner::Ppp, (int)a.owner());
  // Decided on the code byte — the eighth raw byte, the escape before it
  // counted — with nothing sent to the console before it, and every byte
  // of the frame handed to PPP in the order it came.
  TEST_ASSERT_EQUAL(7, f.tookOverAt);
  TEST_ASSERT_EQUAL(0, (int)f.console.size());
  TEST_ASSERT_EQUAL(sizeof(kConfReq), f.ppp.size());
  TEST_ASSERT_EQUAL_MEMORY(kConfReq, f.ppp.data(), sizeof(kConfReq));
}

static void test_a_frame_without_address_and_control_also_counts() {
  // ACFC negotiated away, or a peer that never sends the pair: C0 21 01.
  const uint8_t bare[] = { 0x7E, 0xC0, 0x21, 0x7D, 0x21, 0x7D, 0x22, 0x00, 0x04, 0x7E };
  Arbiter a(true);
  Feed f;
  f.run(a, bare, sizeof(bare));
  TEST_ASSERT_EQUAL((int)Owner::Ppp, (int)a.owner());
  TEST_ASSERT_EQUAL(4, f.tookOverAt);
  TEST_ASSERT_EQUAL(sizeof(bare), f.ppp.size());
}

static void test_a_typed_tilde_is_released_to_the_console_intact() {
  Arbiter a(true);
  Feed f;
  f.run(a, (const uint8_t*)"~hello\n", 7);
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
  TEST_ASSERT_EQUAL(7, (int)f.console.size());
  TEST_ASSERT_EQUAL_MEMORY("~hello\n", f.console.data(), 7);
}

static void test_a_lone_tilde_is_released_when_the_port_goes_quiet() {
  Arbiter a(true);
  Route r = a.feed('~', 1000);
  TEST_ASSERT_EQUAL((int)Sink::Hold, (int)r.sink);
  r = a.idle(1000 + Arbiter::kHoldIdleMs);
  TEST_ASSERT_EQUAL((int)Sink::Hold, (int)r.sink);           // not yet
  r = a.idle(1000 + Arbiter::kHoldIdleMs + 1);
  TEST_ASSERT_EQUAL((int)Sink::Console, (int)r.sink);
  TEST_ASSERT_EQUAL(1, (int)r.len);
  TEST_ASSERT_EQUAL('~', r.data[0]);
  // And once released, nothing is held any more.
  TEST_ASSERT_EQUAL((int)Sink::Hold, (int)a.idle(999999).sink);
  TEST_ASSERT_EQUAL(0, (int)a.idle(999999).len);
}

static void test_other_lcp_codes_and_other_protocols_do_not_take_the_port() {
  // A Terminate-Ack (code 06), an Echo-Request (09), an IP frame (00 21):
  // all PPP, none the start of a session.
  const uint8_t termAck[]  = { 0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x26, 0x7D, 0x21, 0x00, 0x04, 0x7E };
  const uint8_t echoReq[]  = { 0x7E, 0xFF, 0x7D, 0x23, 0xC0, 0x21, 0x7D, 0x29, 0x7D, 0x21, 0x00, 0x08, 0x7E };
  const uint8_t ipFrame[]  = { 0x7E, 0xFF, 0x7D, 0x23, 0x00, 0x21, 0x45, 0x00, 0x00, 0x1C, 0x7E };
  const uint8_t* frames[] = { termAck, echoReq, ipFrame };
  const size_t   sizes[]  = { sizeof(termAck), sizeof(echoReq), sizeof(ipFrame) };
  for (int i = 0; i < 3; i++) {
    Arbiter a(true);
    Feed f;
    f.run(a, frames[i], sizes[i]);
    TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
    TEST_ASSERT_EQUAL(0, (int)f.ppp.size());
    // Noise, but the console's noise: every byte reaches it, the closing
    // flag last — held as the start of a next frame until the port is quiet.
    const Route tail = a.idle(999999);
    TEST_ASSERT_EQUAL(sizes[i], f.console.size() + tail.len);
  }
}

static void test_with_ppp_switched_off_the_frame_is_console_noise() {
  Arbiter a(false);
  Feed f;
  f.run(a, kConfReq, sizeof(kConfReq));
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
  TEST_ASSERT_EQUAL(sizeof(kConfReq), f.console.size());
  TEST_ASSERT_EQUAL(0, (int)f.ppp.size());
  // Switched off halfway through a candidate: what was held goes to the
  // console, not to a PPP that is not allowed to start.
  Arbiter b(true);
  Feed g;
  g.run(b, kConfReq, 6);
  b.allowPpp(false);
  g.run(b, kConfReq + 6, sizeof(kConfReq) - 6);
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)b.owner());
  TEST_ASSERT_EQUAL(sizeof(kConfReq), g.console.size());
}

static void test_after_the_session_ends_the_console_owns_and_pppd_may_return() {
  Arbiter a(true);
  Feed f;
  f.run(a, kConfReq, sizeof(kConfReq));
  TEST_ASSERT_EQUAL((int)Owner::Ppp, (int)a.owner());
  // While PPP owns it, feed() is not the path — but if called, the byte is PPP's.
  TEST_ASSERT_EQUAL((int)Sink::Ppp, (int)a.feed(0x55, 5000).sink);
  TEST_ASSERT_TRUE(a.pppDown());
  TEST_ASSERT_FALSE(a.pppDown());                         // already the console's
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
  Feed g;
  g.run(a, (const uint8_t*)"VERSION\n", 8, 6000);
  TEST_ASSERT_EQUAL(8, (int)g.console.size());
  // pppd with `persist` dials again: the port changes hands again.
  Feed h;
  h.run(a, kConfReq, sizeof(kConfReq), 7000);
  TEST_ASSERT_EQUAL((int)Owner::Ppp, (int)a.owner());
  TEST_ASSERT_EQUAL(sizeof(kConfReq), h.ppp.size());
}

static void test_a_silent_host_is_dead_after_the_idle_limit_and_a_frame_revives_it() {
  Arbiter a(true);
  TEST_ASSERT_FALSE(a.pppIdleDead(1000000));               // the console cannot be idle-dead
  Feed f;
  f.run(a, kConfReq, sizeof(kConfReq), 1000);
  const uint32_t took = 1000 + 7;                          // the byte that decided
  TEST_ASSERT_FALSE(a.pppIdleDead(took + Arbiter::kIdleDeadMs));
  TEST_ASSERT_TRUE(a.pppIdleDead(took + Arbiter::kIdleDeadMs + 1));
  // An LCP echo from the host, at any point, restarts the clock; bytes
  // without a flag in them do not — a stalled frame is not a live peer.
  const uint8_t noFlag[] = { 0x00, 0x01, 0x02 };
  a.pppReceived(noFlag, sizeof(noFlag), took + 20000);
  TEST_ASSERT_TRUE(a.pppIdleDead(took + Arbiter::kIdleDeadMs + 1));
  const uint8_t echo[] = { 0x7E, 0xFF, 0x03, 0xC0, 0x21, 0x09, 0x7E };
  a.pppReceived(echo, sizeof(echo), took + 20000);
  TEST_ASSERT_FALSE(a.pppIdleDead(took + Arbiter::kIdleDeadMs + 1));
  TEST_ASSERT_TRUE(a.pppIdleDead(took + 20000 + Arbiter::kIdleDeadMs + 1));
}

static void test_flags_back_to_back_still_open_a_session() {
  // The closing flag of one frame and the opening flag of the next, or a
  // pppd that sends a flag to settle the line first.
  std::vector<uint8_t> bytes = { 0x7E };
  bytes.insert(bytes.end(), kConfReq, kConfReq + sizeof(kConfReq));
  Arbiter a(true);
  Feed f;
  f.run(a, bytes.data(), bytes.size());
  TEST_ASSERT_EQUAL((int)Owner::Ppp, (int)a.owner());
  TEST_ASSERT_EQUAL(bytes.size(), f.ppp.size());
}

static void test_a_candidate_that_grows_too_long_is_released() {
  // A run of tildes: never a frame, and the console must get them back
  // rather than the arbiter holding them for ever.
  std::vector<uint8_t> tildes(Arbiter::kHoldMax + 4, '~');
  Arbiter a(true);
  Feed f;
  f.run(a, tildes.data(), tildes.size());
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
  // Every byte accounted for: released in batches, held at most kHoldMax.
  TEST_ASSERT_TRUE(f.console.size() >= Arbiter::kHoldMax);
  Route r = a.idle(999999);
  const size_t total = f.console.size() + r.len;
  TEST_ASSERT_EQUAL(tildes.size(), total);
}

static void test_an_escape_that_makes_no_sense_ends_the_candidate() {
  // Two escapes in a row cannot occur in a frame; the bytes are the console's.
  const uint8_t bad[] = { 0x7E, 0xFF, 0x7D, 0x7D, 0x41 };
  Arbiter a(true);
  Feed f;
  f.run(a, bad, sizeof(bad));
  TEST_ASSERT_EQUAL((int)Owner::Console, (int)a.owner());
  TEST_ASSERT_EQUAL(sizeof(bad), f.console.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_console_bytes_pass_through_one_by_one);
  RUN_TEST(test_pppd_opening_frame_takes_the_port_and_arrives_whole);
  RUN_TEST(test_a_frame_without_address_and_control_also_counts);
  RUN_TEST(test_a_typed_tilde_is_released_to_the_console_intact);
  RUN_TEST(test_a_lone_tilde_is_released_when_the_port_goes_quiet);
  RUN_TEST(test_other_lcp_codes_and_other_protocols_do_not_take_the_port);
  RUN_TEST(test_with_ppp_switched_off_the_frame_is_console_noise);
  RUN_TEST(test_after_the_session_ends_the_console_owns_and_pppd_may_return);
  RUN_TEST(test_a_silent_host_is_dead_after_the_idle_limit_and_a_frame_revives_it);
  RUN_TEST(test_flags_back_to_back_still_open_a_session);
  RUN_TEST(test_a_candidate_that_grows_too_long_is_released);
  RUN_TEST(test_an_escape_that_makes_no_sense_ends_the_candidate);
  return UNITY_END();
}
