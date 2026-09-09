// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node. See LICENSE.
//
// Host-native tests for the RNS TCP framing (HDLC.h).
//
// Half of this suite is malformed input. A TCP listener is fed by whoever
// connects to it, so the bytes that reach the deframer are not the bytes some
// sender's escape() produced: they are those bytes truncated by a dropped
// connection, interleaved with a desynced peer's, or chosen by someone who
// would like the node to stop forwarding. The property each malformed case
// ends by asserting is therefore not that the garbage was rejected — it is
// that the next valid frame still decodes, byte for byte. A deframer that
// rejects a bad frame and then never decodes another one has failed.
//
// Where we sit relative to the reference implementation.
//
// "The reference deframer" below always means the same two functions, cited
// once here rather than repeated per test, because bare line numbers into a
// separately checked-out dependency drift: RNS/Interfaces/TCPInterface.py,
// class TCPClientInterface — read_loop()'s standard-HDLC branch (lines 387-411
// in RNS 1.5.0) and check_frame_len() (336-339). It is not a streaming state
// machine. It accumulates into a buffer, finds FLAG..FLAG on the
// *still-escaped* bytes, slices, and then unescapes with two sequential
// non-overlapping bytes.replace() passes.
//
// What agrees, for a stream from a conforming sender: the escape decoding, and
// the frame boundaries. escape() emits only literal bytes, 7D 5E and 7D 5D — a
// bare 7D never occurs — so the first replace pass matches exactly the 7D 5E
// pairs and the second exactly the 7D 5D pairs, which is byte for byte what the
// XOR here does; and both split at every FLAG. On malformed input the escape
// decoding diverges, in three ways, each pinned and explained at the test that
// covers it. Those comments say what each side *decodes*; whether RNS then
// hands the result upward is the separate policy below, which for the short
// payloads used here rejects them.
//
// What deliberately does not agree, on well-formed input: which frames are
// accepted. check_frame_len() rejects frame_len <= RNS.Reticulum.HEADER_MINSIZE
// (19) and frame_len > HW_MTU + ifac_size (262144 by default, with no IFAC
// configured), so RNS accepts payloads of 20..262144 bytes on a default
// interface. This deframer accepts 1..RNS_MTU (500) and counts the over-MTU
// remainder through oversized(). Both divergences are real and neither is a
// framing bug: the 2-byte kGoodPayload every malformed case below recovers
// into is a frame RNS would discard as too short, and a well-formed 501-byte
// payload is one RNS delivers whole and this node drops
// (test_one_byte_over_mtu_is_dropped_and_counted_once). Nothing in this suite
// asserts general agreement — only the two properties named above.
//
// The empty frame that back-to-back FLAGs produce is *not* one of those
// divergences, and is named here so nobody later sets out to fix a difference
// that is not there: read_loop() wraps its whole accept path in
// `if frame_len != 0:`, so RNS skips an empty frame in silence as well — not
// even an invalid_frame() log. Both sides ignore it, for the same reason.
#include <unity.h>
#include <stdio.h>
#include <vector>
#include "HDLC.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Feed a stream into a caller-owned deframer, appending every frame it emits
// to `out`. Taking the Deframer by reference is what lets a test split a
// stream into several feeds and look at oversized() or call reset() between
// them; deframeAll() below is the one-shot form the framing tests use.
static void feedInto(HDLC::Deframer& d, const uint8_t* stream, size_t len,
                     std::vector<std::vector<uint8_t>>& out) {
  for (size_t i = 0; i < len; i++)
    d.feed(stream[i], [&](const uint8_t* p, size_t n) { out.emplace_back(p, p + n); });
}

static std::vector<std::vector<uint8_t>> deframeAll(const uint8_t* stream, size_t len) {
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, len, out);
  return out;
}

// The canonical next-valid-frame every malformed case recovers into.
static const uint8_t kGoodPayload[] = { 0x42, 0x43 };
static const uint8_t kGoodFrame[]   = { 0x7E, 0x42, 0x43, 0x7E };

// The assertion every malformed-input case ends with: hand the deframer that
// just swallowed the malformed bytes one valid frame, and require it back byte
// for byte. It lives here, once, so that no case can quietly omit it or state
// it more weakly than its neighbours.
//
// `expectedNew` is how many frames the valid frame's arrival should add to
// `out`. It is 1 in the ordinary case, and 2 where the malformed bytes left a
// frame open that the valid frame's leading FLAG closes on its way in; those
// cases assert the flushed frame's contents themselves.
static void assertRecovers(HDLC::Deframer& d, std::vector<std::vector<uint8_t>>& out,
                           const char* what, size_t expectedNew = 1) {
  const size_t before = out.size();
  feedInto(d, kGoodFrame, sizeof(kGoodFrame), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(before + expectedNew, out.size(), what);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(sizeof(kGoodPayload), out.back().size(), what);
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(kGoodPayload, out.back().data(),
                                        sizeof(kGoodPayload), what);
}

static void assertFrame(const std::vector<uint8_t>& got, const uint8_t* expect, size_t n,
                        const char* what) {
  TEST_ASSERT_EQUAL_UINT_MESSAGE(n, got.size(), what);
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(expect, got.data(), n, what);
}

// An oversized frame, opened but not closed: a FLAG followed by `over` more
// than RNS_MTU payload bytes, none of which escape. The caller appends the
// closing FLAG, because that is where the drop is counted — for the repetition
// case the closing FLAG is the next partial's opening one. Here so that "what
// counts as oversized" is written once for the whole suite.
static std::vector<uint8_t> oversizedFrame(size_t over = 1) {
  std::vector<uint8_t> f(1 + RNS_MTU + over, 0x01);
  f[0] = 0x7E;
  return f;
}

// A run of non-FLAG bytes comfortably longer than the MTU, with no opening
// FLAG anywhere in it: a non-RNS client's banner, or a connection joined
// mid-transmission, arriving before any frame has been opened.
static std::vector<uint8_t> nonFrameGarbage() {
  return std::vector<uint8_t>(RNS_MTU + 50, 0x01);
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

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
  std::vector<uint8_t> stream = oversizedFrame(10);
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
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, pkts.size(), "worst-case escaped MTU frame");
  TEST_ASSERT_EQUAL_UINT(RNS_MTU, pkts[0].size());
  assertFrame(pkts[0], payload.data(), payload.size(), "worst-case escaped MTU frame");
}

// ---------------------------------------------------------------------------
// Truncation
// ---------------------------------------------------------------------------

// A frame the peer never closed — the connection died mid-write. The payload
// must not reach the sink on the strength of end-of-stream alone: nothing
// downstream can tell a short packet from half a long one.
void test_a_truncated_frame_is_never_delivered() {
  const uint8_t truncated[] = { 0x7E, 0x41, 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, truncated, sizeof(truncated), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "truncated frame delivered without a flag");
  d.reset();
  assertRecovers(d, out, "truncated frame");
}

// reset() is the port-level resync: drop everything buffered, wait for the
// next FLAG. It has no caller in src/ yet — the serial interface is what will
// need it — so this pins the contract before anything depends on it. The bytes
// fed after the reset are exactly the ones that would have completed the
// truncated frame, so any reset() that leaves the parser *inside* the frame
// emits here and fails the count below: a no-op reset() emits 41 42 43, and one
// clearing len but not inFrame emits 43. What it does not discriminate is a
// reset() that clears inFrame and keeps the buffered bytes — the next FLAG
// zeroes len before any byte can be stored, so those bytes are unreachable
// whether or not reset() dropped them.
void test_reset_discards_a_truncated_frames_bytes() {
  const uint8_t truncated[]  = { 0x7E, 0x41, 0x42 };
  const uint8_t completion[] = { 0x43, 0x7E };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, truncated, sizeof(truncated), out);
  d.reset();
  feedInto(d, completion, sizeof(completion), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "reset() left the buffered payload behind");
  assertRecovers(d, out, "reset after truncation");
}

// The same truncation with no reset(): the next frame's opening FLAG closes it
// and 41 42 is delivered as if it had been a whole packet. This is not a
// defect to be fixed but a property of byte-stream framing — with no
// separator, a truncated frame and a complete one are the same bytes — and it
// is pinned so that it stays a decision rather than a surprise. Reticulum's
// own integrity checks are what reject the short packet downstream.
void test_the_next_frames_flag_closes_a_truncated_frame() {
  const uint8_t truncated[] = { 0x7E, 0x41, 0x42 };
  const uint8_t flushed[]   = { 0x41, 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, truncated, sizeof(truncated), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "truncation delivered before the next flag");
  assertRecovers(d, out, "truncation closed by the next flag", 2);
  assertFrame(out[0], flushed, sizeof(flushed), "flushed truncation");
}

// ---------------------------------------------------------------------------
// Escapes
// ---------------------------------------------------------------------------

// ESC followed by a byte that is neither 0x5D nor 0x5E. No conforming sender
// emits this; we XOR unconditionally, so 7D 41 becomes 0x61.
//
// Divergence from the reference deframer (see the header), on malformed input
// only: RNS unescapes with two bytes.replace() passes that match only 7D 5E and
// 7D 5D, so neither matches here and RNS decodes 41 7D 41 42 — both bytes
// literal, four bytes — where we decode 41 61 42. Both are wrong in the sense
// that neither is what the sender meant; neither can be right, because the
// sender did not mean anything. What matters is that recovery is identical:
// both deframers re-open at the same FLAG, and a payload corrupted either way
// fails Reticulum's integrity checks downstream just the same.
void test_an_escape_before_a_non_escapee_is_unescaped_anyway() {
  const uint8_t stream[] = { 0x7E, 0x41, 0x7D, 0x41, 0x42, 0x7E };
  const uint8_t expect[] = { 0x41, 0x61, 0x42 };            // 0x41 ^ 0x20
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "escaped non-escapee");
  assertFrame(out[0], expect, sizeof(expect), "escaped non-escapee");
  assertRecovers(d, out, "escaped non-escapee");
}

// An ESC as the last byte the peer ever sent. It arms the unescape and nothing
// follows: no frame, and in particular no half-escaped byte.
void test_a_dangling_escape_at_end_of_stream_delivers_nothing() {
  const uint8_t stream[] = { 0x7E, 0x41, 0x42, 0x7D };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "dangling escape produced a frame");
  d.reset();
  assertRecovers(d, out, "dangling escape at end of stream");
}

// The same dangling ESC, but the peer carries on. The FLAG clears the pending
// escape, so the escape is simply lost and the payload ahead of it is
// delivered intact.
void test_a_dangling_escape_is_dropped_when_the_next_flag_arrives() {
  const uint8_t stream[]  = { 0x7E, 0x41, 0x42, 0x7D };
  const uint8_t flushed[] = { 0x41, 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "dangling escape produced a frame");
  assertRecovers(d, out, "dangling escape before a flag", 2);
  assertFrame(out[0], flushed, sizeof(flushed), "frame with a dangling escape");
}

// A FLAG where the escaped byte should have been. FLAG is tested first in
// feed(), before the escape state, so it closes the frame and the pending
// escape dies with it.
//
// Divergence from the reference deframer (see the header), on malformed input
// only: RNS searches for the closing FLAG in the still-escaped buffer, so it
// splits at the same byte we do, but the 7D is then inside the slice and
// survives both replace() passes — RNS decodes 41 42 7D, we decode 41 42. The
// frame boundaries agree; only the corrupt payload differs, and it is discarded
// downstream either way. The second frame is byte-identical in both.
void test_a_flag_inside_an_escape_pair_closes_the_frame() {
  const uint8_t stream[] = { 0x7E, 0x41, 0x42, 0x7D, 0x7E, 0x42, 0x43, 0x7E };
  const uint8_t first[]  = { 0x41, 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(2, out.size(), "frames around an escaped flag");
  assertFrame(out[0], first, sizeof(first), "frame cut short by an escaped flag");
  assertFrame(out[1], kGoodPayload, sizeof(kGoodPayload), "frame after an escaped flag");
  assertRecovers(d, out, "escaped flag");
}

// ESC ESC: the second ESC re-takes the escape branch, so the two collapse into
// one pending escape, 5D unescapes to 7D, and the 5E that follows is stored
// raw. Two payload bytes, 7D 5E.
//
// Divergence from the reference deframer (see the header), on malformed input
// only: RNS's first pass looks for 7D 5E and finds none in 7D 7D 5D 5E; its
// second pass rewrites the 7D 5D at offset one, leaving 7D 7D 5E — three bytes
// to our two. Sequential non-overlapping replaces cannot see the pair the
// state machine sees. Recovery is again identical: both deframers close this
// frame at the same FLAG and decode the next one correctly.
void test_a_doubled_escape_collapses_to_one_pending_escape() {
  const uint8_t stream[] = { 0x7E, 0x7D, 0x7D, 0x5D, 0x5E, 0x7E };
  const uint8_t expect[] = { 0x7D, 0x5E };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "doubled escape");
  assertFrame(out[0], expect, sizeof(expect), "doubled escape");
  assertRecovers(d, out, "doubled escape");
}

// ---------------------------------------------------------------------------
// Repetition — a peer that never stops sending malformed frames
// ---------------------------------------------------------------------------

void test_repeated_partial_frames_each_close_on_the_next_flag() {
  const uint8_t partial[] = { 0x7E, 0x41, 0x42 };
  const uint8_t expect[]  = { 0x41, 0x42 };
  const uint8_t closing   = 0x7E;
  const int kPartials     = 3;
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  for (int i = 0; i < kPartials; i++) feedInto(d, partial, sizeof(partial), out);
  feedInto(d, &closing, 1, out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(kPartials, out.size(), "repeated partials delivered");
  for (size_t i = 0; i < out.size(); i++) {
    char what[40];
    snprintf(what, sizeof(what), "repeated partial frame %u", (unsigned)i);
    assertFrame(out[i], expect, sizeof(expect), what);
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(), "partials counted as oversized");
  assertRecovers(d, out, "repeated partials", 1);
}

// Repeated oversized frames must neither be delivered nor accumulate any state
// that outlives them. Note where the counter ticks: on the closing FLAG, which
// for each partial here is the *next* partial's opening FLAG. So after N
// oversized partials only N-1 have been closed and counted; the last is still
// open, and the valid frame's leading FLAG is what closes and counts it. An
// oversized frame a peer never closes is not counted until it is.
void test_repeated_oversized_partials_are_each_counted() {
  const int kPartials = 3;
  std::vector<uint8_t> stream;
  for (int i = 0; i < kPartials; i++) {
    const std::vector<uint8_t> partial = oversizedFrame();       // never closed
    stream.insert(stream.end(), partial.begin(), partial.end());
  }
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream.data(), stream.size(), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "an oversized frame was delivered");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kPartials - 1, d.oversized(), "closed oversized frames");
  assertRecovers(d, out, "repeated oversized partials");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(kPartials, d.oversized(), "oversized frames after recovery");
}

// ---------------------------------------------------------------------------
// Garbage and resynchronisation
// ---------------------------------------------------------------------------

// Bytes before the first FLAG cannot reach a delivered frame, and not because
// of any one guard: the opening FLAG clears len, escaped and overflow before a
// payload byte can be stored, and the sink is separately gated on inFrame. So
// whatever the 7D and the 7D 5E pair in here do to the parse state, the frame
// that follows decodes as if they had never arrived — this case holds with
// feed()'s !inFrame early return deleted, and the trailing 0x00 means nothing
// is left armed either. It is a statement about the contract, not a guard test.
// The guard's real effect is on the oversized counter, pinned by
// test_long_garbage_before_the_first_flag_does_not_count_as_oversized; the one
// garbage position that does change what is delivered is pinned by
// test_garbage_between_two_frames_is_delivered_as_a_frame.
void test_garbage_before_the_first_flag_is_discarded() {
  const uint8_t garbage[] = { 0x55, 0x7D, 0x5E, 0xAA, 0x7D, 0x5D, 0x00 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, garbage, sizeof(garbage), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "garbage before the first flag produced a frame");
  assertRecovers(d, out, "garbage before the first flag");
}

// reset() puts a deframer back outside a frame mid-stream: everything until the
// next FLAG is garbage again, and unreachable for the same reason.
void test_garbage_after_a_reset_is_discarded() {
  const uint8_t garbage[] = { 0x55, 0x7D, 0xAA, 0x5D };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, kGoodFrame, sizeof(kGoodFrame), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "the frame before the resync was lost");
  d.reset();
  feedInto(d, garbage, sizeof(garbage), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "garbage after reset() produced a frame");
  assertRecovers(d, out, "garbage after reset");
}

// Garbage strictly *between* two frames — neither before the first FLAG nor
// after a reset(), and the only garbage position with an observable effect. A
// closing FLAG is not a separator: it also opens the next frame, so bytes
// arriving after it are already inside one and the following FLAG delivers
// them. 7E 41 7E 55 66 7E 42 7E is three frames, not two frames with a
// discarded run between them.
//
// Operationally the junk becomes a packet in tcpInRing and a
// g_stats.tcpRxPackets tick (RetiTransportServer::onData()); Reticulum's own
// integrity checks are what reject it downstream.
//
// The reference deframer slices identically — frame_buffer =
// frame_buffer[frame_end:] keeps the closing FLAG as the next frame's opener —
// so it finds these same three frames. The boundaries are the compatibility
// claim, and "fixing" this into a discard would be the divergence. What RNS
// then does with them is its length policy, not framing: all three are under
// HEADER_MINSIZE, so it logs them as invalid frames and drops them, exactly as
// it would drop this suite's own 2-byte kGoodPayload (see the header).
void test_garbage_between_two_frames_is_delivered_as_a_frame() {
  const uint8_t stream[] = { 0x7E, 0x41, 0x7E, 0x55, 0x66, 0x7E, 0x42, 0x7E };
  const uint8_t first[]  = { 0x41 };
  const uint8_t junk[]   = { 0x55, 0x66 };
  const uint8_t last[]   = { 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(3, out.size(), "inter-frame junk was not framed");
  assertFrame(out[0], first, sizeof(first), "frame before the junk");
  assertFrame(out[1], junk, sizeof(junk), "the junk itself, framed");
  assertFrame(out[2], last, sizeof(last), "frame after the junk");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(), "inter-frame junk counted as oversized");
  assertRecovers(d, out, "garbage between two frames");
}

// Two frames sharing one FLAG. The existing round-trip test covers the doubled
// form two frame() calls produce; a peer writing the shared form is equally
// legal and must decode to the same two frames.
void test_two_frames_sharing_one_flag_both_decode() {
  const uint8_t stream[] = { 0x7E, 0x41, 0x7E, 0x42, 0x7E };
  const uint8_t first[]  = { 0x41 };
  const uint8_t second[] = { 0x42 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream, sizeof(stream), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(2, out.size(), "shared-flag pair");
  assertFrame(out[0], first, sizeof(first), "first of a shared-flag pair");
  assertFrame(out[1], second, sizeof(second), "second of a shared-flag pair");
  assertRecovers(d, out, "shared flag");
}

// Connecting to a peer already mid-transmission: the stream begins inside a
// frame, with no opening FLAG to be found.
void test_a_stream_beginning_mid_frame_discards_the_leading_bytes() {
  const uint8_t leading[] = { 0x41, 0x42, 0x43 };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, leading, sizeof(leading), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "mid-frame leading bytes produced a frame");
  assertRecovers(d, out, "stream beginning mid-frame");
}

// The same, landing in the middle of an escape pair: a stray 5E that was the
// tail of one, then a whole 7D 5D. Neither survives into a frame: the opening
// FLAG of the next one clears the parse state before any payload byte lands.
void test_a_stream_beginning_mid_escape_pair_discards_the_leading_bytes() {
  const uint8_t leading[] = { 0x5E, 0x7D, 0x5D };
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, leading, sizeof(leading), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "mid-escape leading bytes produced a frame");
  assertRecovers(d, out, "stream beginning mid-escape-pair");
}

// ---------------------------------------------------------------------------
// The MTU boundary, and the counter that reports it
// ---------------------------------------------------------------------------

// Exactly MTU bytes is the largest accepted frame: delivered whole, and not
// counted. The counter is what tells an operator that a peer speaking a bigger
// MTU is not simply idle, so a false tick here would be a misleading report.
void test_exactly_mtu_payload_is_delivered_and_not_counted() {
  std::vector<uint8_t> payload(RNS_MTU, 0x01);
  std::vector<uint8_t> stream;
  stream.push_back(0x7E);
  stream.insert(stream.end(), payload.begin(), payload.end());
  stream.push_back(0x7E);
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream.data(), stream.size(), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "exactly-MTU frame");
  assertFrame(out[0], payload.data(), payload.size(), "exactly-MTU frame");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(), "an MTU-sized frame was counted oversized");
  assertRecovers(d, out, "exactly-MTU frame");
}

// One byte more: dropped, and counted exactly once however far over it went.
void test_one_byte_over_mtu_is_dropped_and_counted_once() {
  std::vector<uint8_t> stream = oversizedFrame();
  stream.push_back(0x7E);
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream.data(), stream.size(), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "an over-MTU frame was delivered");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.oversized(), "over-MTU frames counted");
  assertRecovers(d, out, "one byte over MTU");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.oversized(), "a good frame changed the counter");
}

// reset() clears the parse state and deliberately keeps the counter: it is a
// lifetime total for the link, and a resync is not a reason to forget that a
// peer has been sending frames this node cannot carry.
void test_reset_does_not_clear_the_oversized_counter() {
  std::vector<uint8_t> stream = oversizedFrame();
  stream.push_back(0x7E);
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, stream.data(), stream.size(), out);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.oversized(), "over-MTU frame counted before reset");
  d.reset();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.oversized(), "reset() cleared the oversized counter");
  assertRecovers(d, out, "reset after an oversized frame");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, d.oversized(), "counter changed after reset and recovery");
}

// The other half of that contract. Every oversized case above closes its frame,
// and the closing FLAG clears overflow on its way through — so at the moment
// those tests call reset(), there is no overflow left to clear, and a reset()
// that kept it would be indistinguishable from one that cleared it. Here the
// oversized frame is never closed, so overflow is still live when reset()
// runs. If it survives, the recovery frame's opening FLAG counts a drop that
// never happened; the frame itself still arrives intact, so the counter is the
// only place it shows.
void test_reset_discards_an_active_overflow() {
  const std::vector<uint8_t> partial = oversizedFrame();       // never closed
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, partial.data(), partial.size(), out);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(), "an unclosed frame was counted early");
  d.reset();
  assertRecovers(d, out, "reset with a live overflow");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(
      0, d.oversized(), "reset() left overflow set: the next FLAG counted a frame that never was");
}

// What feed()'s !inFrame early return actually protects is this counter, not
// the payload: delivery is gated on inFrame separately, so deleting the return
// changes nothing a frame-content test can see. It changes this. Without it, a
// run of more than RNS_MTU non-FLAG bytes arriving before any frame has opened
// walks into the length check, sets `overflow`, and the next FLAG then ticks
// dropped++ for bytes that were never a frame at all.
//
// oversized() is the only thing separating "this peer frames packets bigger
// than we carry" from "this peer is idle" (Deframer's class comment), and
// RetiTransportServer::onData() logs a named peer off it. A tick here would
// blame a peer that did nothing wrong: a non-RNS client's banner, or handshake
// bytes ahead of its first frame, would be reported as traffic this node
// dropped.
void test_long_garbage_before_the_first_flag_does_not_count_as_oversized() {
  const std::vector<uint8_t> garbage = nonFrameGarbage();
  const uint8_t opening = 0x7E;
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, garbage.data(), garbage.size(), out);
  feedInto(d, &opening, 1, out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(0, out.size(), "pre-flag garbage produced a frame");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(),
                                   "garbage before the first flag counted as an oversized frame");
  assertRecovers(d, out, "long garbage before the first flag");
}

// The same after reset(): a resync puts the deframer back outside a frame, so
// the run that follows is not a frame either and must not be counted as one.
void test_long_garbage_after_a_reset_does_not_count_as_oversized() {
  const std::vector<uint8_t> garbage = nonFrameGarbage();
  const uint8_t opening = 0x7E;
  HDLC::Deframer d;
  std::vector<std::vector<uint8_t>> out;
  feedInto(d, kGoodFrame, sizeof(kGoodFrame), out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "the frame before the resync was lost");
  d.reset();
  feedInto(d, garbage.data(), garbage.size(), out);
  feedInto(d, &opening, 1, out);
  TEST_ASSERT_EQUAL_UINT_MESSAGE(1, out.size(), "post-reset garbage produced a frame");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.oversized(),
                                   "garbage after a reset counted as an oversized frame");
  assertRecovers(d, out, "long garbage after a reset");
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_frame_escapes_flag_and_esc);
  RUN_TEST(test_frame_rejects_small_buffer);
  RUN_TEST(test_roundtrip_two_packets_and_garbage);
  RUN_TEST(test_oversized_frame_is_dropped_and_parser_recovers);
  RUN_TEST(test_mtu_sized_frame_passes);
  RUN_TEST(test_a_truncated_frame_is_never_delivered);
  RUN_TEST(test_reset_discards_a_truncated_frames_bytes);
  RUN_TEST(test_the_next_frames_flag_closes_a_truncated_frame);
  RUN_TEST(test_an_escape_before_a_non_escapee_is_unescaped_anyway);
  RUN_TEST(test_a_dangling_escape_at_end_of_stream_delivers_nothing);
  RUN_TEST(test_a_dangling_escape_is_dropped_when_the_next_flag_arrives);
  RUN_TEST(test_a_flag_inside_an_escape_pair_closes_the_frame);
  RUN_TEST(test_a_doubled_escape_collapses_to_one_pending_escape);
  RUN_TEST(test_repeated_partial_frames_each_close_on_the_next_flag);
  RUN_TEST(test_repeated_oversized_partials_are_each_counted);
  RUN_TEST(test_garbage_before_the_first_flag_is_discarded);
  RUN_TEST(test_garbage_after_a_reset_is_discarded);
  RUN_TEST(test_garbage_between_two_frames_is_delivered_as_a_frame);
  RUN_TEST(test_two_frames_sharing_one_flag_both_decode);
  RUN_TEST(test_a_stream_beginning_mid_frame_discards_the_leading_bytes);
  RUN_TEST(test_a_stream_beginning_mid_escape_pair_discards_the_leading_bytes);
  RUN_TEST(test_exactly_mtu_payload_is_delivered_and_not_counted);
  RUN_TEST(test_one_byte_over_mtu_is_dropped_and_counted_once);
  RUN_TEST(test_reset_does_not_clear_the_oversized_counter);
  RUN_TEST(test_reset_discards_an_active_overflow);
  RUN_TEST(test_long_garbage_before_the_first_flag_does_not_count_as_oversized);
  RUN_TEST(test_long_garbage_after_a_reset_does_not_count_as_oversized);
  return UNITY_END();
}
