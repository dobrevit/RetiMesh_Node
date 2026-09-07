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


// BootRecord: the one thing a run leaves behind for the next one to read.
//
// Everything here is about a failure that is silent either way. A record
// misread as valid reports whatever a dead RTC domain left lying in memory as
// though it were measurement; a record misread as invalid throws away the only
// evidence a panic left. Neither shows up as a crash, a warning, or a failing
// build — the node simply reports a confident number that is not true, which
// is the exact fault that made a Wireless Bridge look healthy for six days.

#include <unity.h>
#include <string.h>
#include "BootRecord.h"

using namespace Diag;

// The magic values this layout has had. Kept literal rather than derived: the
// point of the check is that an old record is refused, so a test that computed
// the old value from the new one would refuse nothing.
static constexpr uint32_t kMagicRtm1 = 0x52544D31;
static constexpr uint32_t kMagicRtm2 = 0x52544D32;

static Record valid(uint32_t uptimeS = 3600, uint32_t allocs = 0, uint32_t caught = 0) {
  Record r{};
  detail::beginRun(r);
  r.uptimeS       = uptimeS;
  r.allocFailures = allocs;
  r.caught        = caught;
  return r;
}

// --- what the magic decides -------------------------------------------------

static void test_an_unwritten_record_is_not_evidence() {
  // A cold start: RTC memory holds whatever the rail dropping left in it. The
  // fields may be any value at all, and none of them is a measurement.
  Record r{};
  memset(&r, 0xA5, sizeof(r));
  const Previous p = readPrevious(r);
  TEST_ASSERT_FALSE(p.known);
}

static void test_an_all_zero_record_is_not_evidence_either() {
  // The other shape noise takes, and the more dangerous one: every field reads
  // as a plausible zero. Without the magic this is "the previous run lasted no
  // time and failed no allocations", which is a statement, not an absence.
  Record r{};
  memset(&r, 0x00, sizeof(r));
  const Previous p = readPrevious(r);
  TEST_ASSERT_FALSE(p.known);
  TEST_ASSERT_EQUAL_UINT32(0, p.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(0, p.allocFailures);
}

static void test_an_rtm2_record_is_read_for_what_it_does_carry() {
  // RTM2's sixteen bytes are a strict prefix of this layout, so its run length
  // and restart marks are exactly where they are looked for. Refusing the
  // whole record instead reported the *upgrade reboot itself* as a power cut,
  // on the surface whose only job is telling those two apart.
  Record r = valid(7200, 11, 4);
  r.restart = RestartMarks{500, 600};
  r.magic = kMagicRtm2;
  const Previous p = readPrevious(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_EQUAL_UINT32(7200, p.uptimeS);
  TEST_ASSERT_TRUE(p.restartMarked);
  TEST_ASSERT_EQUAL_UINT32(500, p.restart.entryMs);
}

static void test_an_rtm2_records_fault_counts_are_not_invented() {
  // Past RTM2's sixteen bytes is uninitialised RTC memory. Whatever it holds
  // is not a measurement, and reporting it would be worse than reporting
  // nothing: this is the field an operator acts on.
  Record r = valid(7200, 11, 4);
  r.magic = kMagicRtm2;
  const Previous p = readPrevious(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_FALSE(p.faultsKnown);
}

static void test_an_rtm1_record_is_still_refused() {
  // RTM1 predates the restart marks, so its bytes at that offset are not
  // marks and would be timed as though they were.
  Record r = valid(7200, 11, 4);
  r.magic = kMagicRtm1;
  TEST_ASSERT_FALSE(readPrevious(r).known);
}

static void test_a_current_record_knows_its_fault_counts() {
  TEST_ASSERT_TRUE(readPrevious(valid(10, 0, 0)).faultsKnown);
}

static void test_the_magic_is_the_documented_value_and_is_new() {
  TEST_ASSERT_EQUAL_UINT32(0x52544D33, kRecordMagic);
  TEST_ASSERT_NOT_EQUAL(kMagicRtm1, kRecordMagic);
  TEST_ASSERT_NOT_EQUAL(kMagicRtm2, kRecordMagic);
}

static void test_one_wrong_bit_in_the_magic_refuses_the_record() {
  for (int bit = 0; bit < 32; bit++) {
    Record r = valid(60, 1, 1);
    r.magic ^= (uint32_t)1u << bit;
    if (r.magic == kMagicRtm2) continue;      // the one neighbour that is ours
    TEST_ASSERT_FALSE(readPrevious(r).known);
  }
}

// --- what a valid record reports --------------------------------------------

static void test_a_valid_record_reports_the_run_that_wrote_it() {
  const Record r = valid(15043, 23, 8);
  const Previous p = readPrevious(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_EQUAL_UINT32(15043, p.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(23, p.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(8, p.caught);
}

static void test_a_clean_run_reports_zero_faults_and_says_it_knows() {
  // The distinction the whole file exists for: "no allocation failed" and "we
  // cannot say whether one did" are different answers and must not share a
  // representation.
  const Previous p = readPrevious(valid(86400, 0, 0));
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_EQUAL_UINT32(0, p.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(0, p.caught);
}

static void test_the_counts_are_carried_at_full_width() {
  // An allocation storm counts fast, and a saturating or truncated count would
  // understate exactly the run worth understanding.
  const Previous p = readPrevious(valid(1, 0xFFFFFFFFu, 0xFFFFFFFEu));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, p.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFEu, p.caught);
}

static void test_reading_does_not_disturb_the_record() {
  Record r = valid(999, 3, 2);
  const Record before = r;
  (void)readPrevious(r);
  TEST_ASSERT_EQUAL_MEMORY(&before, &r, sizeof(Record));
}

// --- claiming the record for this run ---------------------------------------

static void test_begin_run_stamps_the_magic_and_clears_the_run() {
  Record r{};
  memset(&r, 0xFF, sizeof(r));
  detail::beginRun(r);
  TEST_ASSERT_EQUAL_UINT32(kRecordMagic, r.magic);
  TEST_ASSERT_EQUAL_UINT32(0, r.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(0, r.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(0, r.caught);
  TEST_ASSERT_EQUAL_UINT32(0, r.restart.entryMs);
  TEST_ASSERT_EQUAL_UINT32(0, r.restart.persistMs);
}

static void test_claiming_before_reading_destroys_the_evidence() {
  // The failure this contract exists to prevent, written out as the thing that
  // actually goes wrong rather than as the order that goes right. Sequencing
  // it the wrong way round loses the dead run entirely — and it loses it
  // silently, reporting a confident zero rather than an error.
  Record r = valid(4242, 9, 5);
  detail::beginRun(r);                      // the mistake
  const Previous tooLate = readPrevious(r);
  TEST_ASSERT_TRUE(tooLate.known);          // still looks like a valid record
  TEST_ASSERT_EQUAL_UINT32(0, tooLate.uptimeS);      // ...reporting nothing
  TEST_ASSERT_EQUAL_UINT32(0, tooLate.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(0, tooLate.caught);
}

static void test_claim_run_reads_and_claims_in_the_right_order() {
  // The pairing callers are given, so the order cannot be got wrong in a
  // caller at all. Diag::begin() uses this and nothing else.
  Record r = valid(4242, 9, 5);
  const Previous kept = claimRun(r);
  TEST_ASSERT_TRUE(kept.known);
  TEST_ASSERT_EQUAL_UINT32(4242, kept.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(9, kept.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(5, kept.caught);
  // ...and the record now belongs to this run.
  TEST_ASSERT_EQUAL_UINT32(kRecordMagic, r.magic);
  TEST_ASSERT_EQUAL_UINT32(0, r.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(0, r.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(0, r.caught);
}

static void test_claim_run_on_a_dropped_rtc_domain_reports_unknown_and_still_claims() {
  // A cold start: nothing to carry, but the record must still be claimed or
  // this run would write into bytes it never validated.
  Record r{};
  memset(&r, 0xA5, sizeof(r));
  const Previous p = claimRun(r);
  TEST_ASSERT_FALSE(p.known);
  TEST_ASSERT_FALSE(p.faultsKnown);
  TEST_ASSERT_EQUAL_UINT32(kRecordMagic, r.magic);
  TEST_ASSERT_EQUAL_UINT32(0, r.allocFailures);
}

static void test_claiming_an_rtm2_record_upgrades_it_in_place() {
  // The upgrade path end to end: carry what RTM2 had, refuse to invent what it
  // did not, and leave the record stamped as this layout's.
  Record r = valid(7200, 0, 0);
  r.magic = kMagicRtm2;
  const Previous p = claimRun(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_FALSE(p.faultsKnown);
  TEST_ASSERT_EQUAL_UINT32(7200, p.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(kRecordMagic, r.magic);
  TEST_ASSERT_TRUE(readPrevious(r).faultsKnown);   // the next boot gets counts
}

static void test_a_second_claim_carries_this_run_not_the_one_before() {
  // Two restarts in a row: each boot must report the run immediately before
  // it, never an older one still lying in the fields.
  Record r{};
  claimRun(r);
  r.uptimeS = 100; r.allocFailures = 1; r.caught = 0;      // run A
  const Previous a = claimRun(r);
  r.uptimeS = 200; r.allocFailures = 7; r.caught = 2;      // run B
  const Previous b = claimRun(r);
  TEST_ASSERT_EQUAL_UINT32(100, a.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(1, a.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(200, b.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(7, b.allocFailures);
}

static void test_a_record_written_then_read_round_trips() {
  Record r{};
  detail::beginRun(r);
  r.uptimeS = 61; r.allocFailures = 2; r.caught = 1;
  r.restart = RestartMarks{1000, 1200};
  const Previous p = readPrevious(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_EQUAL_UINT32(61, p.uptimeS);
  TEST_ASSERT_EQUAL_UINT32(2, p.allocFailures);
  TEST_ASSERT_EQUAL_UINT32(1, p.caught);
  TEST_ASSERT_TRUE(p.restartMarked);
  TEST_ASSERT_EQUAL_UINT32(1000, p.restart.entryMs);
}

// --- the restart marks ------------------------------------------------------

static void test_a_persist_mark_without_an_entry_mark_is_not_a_restart() {
  // A shape the type permits and a stamping bug could produce: the hand-over
  // was recorded but the entry was not. There is nothing to measure from, and
  // inventing a duration out of the RTC clock would be worse than saying so.
  Record r = valid(50, 0, 0);
  r.restart = RestartMarks{0, 1234};
  const Previous p = readPrevious(r);
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_FALSE(p.restartMarked);
  TEST_ASSERT_FALSE(restartTiming(RestartMarks{0, 1234}, 9999).known);
}

static void test_a_full_width_uptime_survives_the_record() {
  const Previous p = readPrevious(valid(0xFFFFFFFFu, 0, 0));
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, p.uptimeS);
}

static void test_a_run_that_was_not_deliberately_restarted_marks_nothing() {
  const Previous p = readPrevious(valid(100, 0, 0));
  TEST_ASSERT_TRUE(p.known);
  TEST_ASSERT_FALSE(p.restartMarked);
}

static void test_no_marks_means_no_timing_rather_than_a_timing_of_zero() {
  const RestartTiming t = restartTiming(RestartMarks{0, 0}, 500000);
  TEST_ASSERT_FALSE(t.known);
  TEST_ASSERT_EQUAL_UINT32(0, t.toPersistMs);
  TEST_ASSERT_EQUAL_UINT32(0, t.toBootMs);
}

static void test_a_restart_without_a_persist_step_is_timed_from_entry() {
  // Every board but the composite USB device: there is no hand-over to time,
  // so the whole restart is entry-to-boot.
  const RestartTiming t = restartTiming(RestartMarks{10000, 0}, 10850);
  TEST_ASSERT_TRUE(t.known);
  TEST_ASSERT_EQUAL_UINT32(0, t.toPersistMs);
  TEST_ASSERT_EQUAL_UINT32(850, t.toBootMs);
}

static void test_a_restart_with_a_persist_step_times_both_halves() {
  const RestartTiming t = restartTiming(RestartMarks{10000, 10120}, 10850);
  TEST_ASSERT_TRUE(t.known);
  TEST_ASSERT_EQUAL_UINT32(120, t.toPersistMs);
  TEST_ASSERT_EQUAL_UINT32(730, t.toBootMs);   // from the persist mark, not entry
}

static void test_a_restart_across_the_rtc_wrap_is_still_a_short_restart() {
  // The RTC millisecond clock wraps about every 49 days. A node that restarts
  // across the wrap took the same 850 ms it always does, and reporting ~4.29
  // billion would make a routine reboot look like the longest event on record.
  const uint32_t entry = 0xFFFFFF00u;          // 256 ms before the wrap
  const RestartTiming t = restartTiming(RestartMarks{entry, 0}, 0x00000252u);
  TEST_ASSERT_TRUE(t.known);
  TEST_ASSERT_EQUAL_UINT32(850, t.toBootMs);
}

static void test_the_persist_half_also_survives_the_wrap() {
  const RestartTiming t = restartTiming(RestartMarks{0xFFFFFF00u, 0x00000050u}, 0x00000252u);
  TEST_ASSERT_TRUE(t.known);
  TEST_ASSERT_EQUAL_UINT32(336, t.toPersistMs);   // 0x50 - 0xFFFFFF00 in uint32
  TEST_ASSERT_EQUAL_UINT32(514, t.toBootMs);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_unwritten_record_is_not_evidence);
  RUN_TEST(test_an_all_zero_record_is_not_evidence_either);
  RUN_TEST(test_an_rtm2_record_is_read_for_what_it_does_carry);
  RUN_TEST(test_an_rtm2_records_fault_counts_are_not_invented);
  RUN_TEST(test_an_rtm1_record_is_still_refused);
  RUN_TEST(test_a_current_record_knows_its_fault_counts);
  RUN_TEST(test_the_magic_is_the_documented_value_and_is_new);
  RUN_TEST(test_one_wrong_bit_in_the_magic_refuses_the_record);
  RUN_TEST(test_a_valid_record_reports_the_run_that_wrote_it);
  RUN_TEST(test_a_clean_run_reports_zero_faults_and_says_it_knows);
  RUN_TEST(test_the_counts_are_carried_at_full_width);
  RUN_TEST(test_reading_does_not_disturb_the_record);
  RUN_TEST(test_begin_run_stamps_the_magic_and_clears_the_run);
  RUN_TEST(test_claiming_before_reading_destroys_the_evidence);
  RUN_TEST(test_claim_run_reads_and_claims_in_the_right_order);
  RUN_TEST(test_claim_run_on_a_dropped_rtc_domain_reports_unknown_and_still_claims);
  RUN_TEST(test_claiming_an_rtm2_record_upgrades_it_in_place);
  RUN_TEST(test_a_second_claim_carries_this_run_not_the_one_before);
  RUN_TEST(test_a_persist_mark_without_an_entry_mark_is_not_a_restart);
  RUN_TEST(test_a_full_width_uptime_survives_the_record);
  RUN_TEST(test_a_record_written_then_read_round_trips);
  RUN_TEST(test_a_run_that_was_not_deliberately_restarted_marks_nothing);
  RUN_TEST(test_no_marks_means_no_timing_rather_than_a_timing_of_zero);
  RUN_TEST(test_a_restart_without_a_persist_step_is_timed_from_entry);
  RUN_TEST(test_a_restart_with_a_persist_step_times_both_halves);
  RUN_TEST(test_a_restart_across_the_rtc_wrap_is_still_a_short_restart);
  RUN_TEST(test_the_persist_half_also_survives_the_wrap);
  return UNITY_END();
}
