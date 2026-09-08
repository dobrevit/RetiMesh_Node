# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
#
# hil_frames.py's channel arithmetic, its loss accounting, and the statistics
# that turn the second into a verdict.
#
# The first of those is a second copy of a rule the firmware already owns, which
# this project otherwise forbids, and the whole first section here exists to pay
# for it: every mirrored constant is read back out of the C++ that owns it, so a
# band table edited in `src/radio/Airtime.cpp` or a preamble floor moved in
# `src/Config.h` fails a host test in under a second instead of producing a
# bench run that paced a transmitter against a limit the node had stopped
# obeying. A mirror nobody checks is not a mirror, it is a fork.
#
# The second is where a harness quietly lies. A window in which the node was
# transmitting, or in which the sender never got the frame onto the radio, did
# not put the question — and counting it as a delivery flatters the link while
# counting it as a loss condemns it. Those cases have tests of their own because
# both wrong answers look like a working harness.

import importlib.util
import json
import os
import re
import struct
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.join(_HERE, os.pardir)
_ROOT = os.path.abspath(os.path.join(_TOOLS, os.pardir))
_HIL = os.path.join(_TOOLS, "hil_frames.py")

_spec = importlib.util.spec_from_file_location("hil_frames", _HIL)
hf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hf)


def _source(*parts) -> str:
    with open(os.path.join(_ROOT, *parts), encoding="utf-8") as fh:
        return fh.read()


def _f32(x: float) -> float:
    """The same value after a round trip through a 32-bit float."""
    return struct.unpack("f", struct.pack("f", x))[0]


# ===========================================================================
#  The mirror, checked against what it mirrors
# ===========================================================================

class TheFirmwareStillSaysWhatThisToolAssumes(unittest.TestCase):

    def test_the_eu_sub_band_table_matches_the_firmwares(self):
        # Airtime.cpp is the authority on which sub-band a channel falls in and
        # what it may transmit there. This tool paces a real transmitter against
        # that table for hours at a time, so the two must not be allowed to
        # drift: an allowance edited there and not here is an illegal run.
        text = _source("src", "radio", "Airtime.cpp")
        block = re.search(r"kEuBands\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        self.assertIsNotNone(block, "kEuBands is no longer in Airtime.cpp")
        rows = re.findall(r"\{\s*([\d.]+)f,\s*([\d.]+)f,\s*(\d+),\s*\"([^\"]*)\"",
                          block.group(1))
        self.assertEqual(len(rows), len(hf.EU_BANDS))
        for (lo, hi, bp, name), mine in zip(rows, hf.EU_BANDS):
            self.assertAlmostEqual(float(lo), mine[0], places=3)
            self.assertAlmostEqual(float(hi), mine[1], places=3)
            self.assertEqual(int(bp), mine[2])
            self.assertEqual(name, mine[3])

    def test_the_radiolib_mirror_constants_match_airtimes(self):
        # These four are RadioLib 7.7.1's, mirrored once in Airtime.h so that a
        # driver bump shows up as a diff. Mirroring them a second time here is
        # only safe while this test holds them together.
        text = _source("src", "radio", "Airtime.h")
        for name in ("RX_DC_MIN_SYMBOLS_SF7", "RX_DC_MIN_SYMBOLS_SF6",
                     "RX_DC_TRANSITION_US", "RX_DC_COMPENSATION_US",
                     "RX_DC_TCXO_DELAY_US", "DUTY_MARGIN_PCT"):
            m = re.search(r"static const \w+\s+%s\s*=\s*([0-9]+)" % name, text)
            self.assertIsNotNone(m, "%s is no longer declared in Airtime.h" % name)
            self.assertEqual(int(m.group(1)), getattr(hf, name), name)
        m = re.search(r"RX_DC_PERIOD_RAW_MAX\s*=\s*(0x[0-9A-Fa-f]+)", text)
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1), 16), hf.RX_DC_PERIOD_RAW_MAX)

    def test_the_shipped_channel_and_preamble_floor_match_config(self):
        # The preamble floor is the number the sleep window is sized on, so a
        # change to it changes what this harness is proving. The channel matters
        # for a different reason: it decides which sub-band the run is paced
        # against, and 869.525 MHz sits in the only 10 % allowance in the plan.
        text = _source("src", "Config.h")
        wanted = {"RF_FREQ_MHZ": hf.RF_FREQ_MHZ, "RF_BW_KHZ": hf.RF_BW_KHZ,
                  "RF_SF": hf.RF_SF, "RF_CR": hf.RF_CR,
                  "RF_PREAMBLE_SYMS": hf.RF_PREAMBLE_SYMS}
        for name, mine in wanted.items():
            m = re.search(r"#define\s+%s\s+([0-9.]+)" % name, text)
            self.assertIsNotNone(m, "%s is no longer defined in Config.h" % name)
            self.assertAlmostEqual(float(m.group(1)), float(mine), places=4, msg=name)

    def test_the_bandwidth_list_matches_radiocaps(self):
        # The plan narrows SF7 and SF8 onto channels that are not the shipped
        # one, and a bandwidth the chip does not offer is a settings error the
        # node refuses mid-sweep, hours in.
        text = _source("src", "radio", "RadioCaps.cpp")
        block = re.search(r"kBwSubGhz\[\]\s*=\s*\{(.*?)\};", text, re.S)
        self.assertIsNotNone(block)
        values = [float(v) for v in re.findall(r"([0-9.]+)f", block.group(1))]
        self.assertEqual(values[-1], 0.0, "the terminator is gone from kBwSubGhz")
        self.assertEqual(tuple(values[:-1]), hf.SX126X_BANDWIDTHS)

    def test_every_bandwidth_in_the_plan_is_one_the_chip_has(self):
        for point in hf.build_plan("claim", 10, 10):
            self.assertIn(point.bw_khz, hf.SX126X_BANDWIDTHS,
                          "SF%d is planned on %g kHz, which no SX1262 offers"
                          % (point.sf, point.bw_khz))

    def test_the_rnode_framing_byte_matches_our_own_header_length(self):
        # RNode prepends one byte to every LoRa packet; this firmware mirrors
        # that as LORA_HEADER_LEN, which is the in-repo statement of the same
        # fact. Pinned against it rather than against the sibling RNode checkout,
        # which is not present in CI.
        m = re.search(r"#define\s+LORA_HEADER_LEN\s+([0-9]+)", _source("src", "Config.h"))
        self.assertIsNotNone(m)
        self.assertEqual(int(m.group(1)), hf.RNODE_HEADER_BYTES)

    def test_double_precision_here_agrees_with_the_firmwares_float32(self):
        # The firmware computes the symbol time in 32-bit float and then
        # truncates it, and the truncated figure is what the engagement
        # threshold is compared against — so a bandwidth where the two
        # precisions land either side of an integer would make this tool predict
        # a mode the node will not arm. They agree today across the whole
        # accepted space; this fails if a future bandwidth breaks that.
        for sf in range(5, 13):
            for bw in hf.SX126X_BANDWIDTHS:
                theirs = int(_f32(_f32(float(10000 << sf)) / _f32(10.0 * _f32(bw))))
                mine = int((10000 << sf) / (10.0 * bw))
                self.assertEqual(theirs, mine, "SF%d at %g kHz" % (sf, bw))


# ===========================================================================
#  Airtime and the duty-cycle prediction
# ===========================================================================

class AirtimeIsTheSemtechFormula(unittest.TestCase):

    def test_the_symbol_time_is_two_to_the_sf_over_the_bandwidth(self):
        self.assertAlmostEqual(hf.symbol_time_ms(7, 125.0), 1.024, places=6)
        self.assertAlmostEqual(hf.symbol_time_ms(12, 125.0), 32.768, places=6)
        self.assertAlmostEqual(hf.symbol_time_ms(8, 62.5), 4.096, places=6)

    def test_a_worked_frame_matches_the_datasheet(self):
        # 28 bytes on air at SF12/125 kHz, CR 4/5, 18-symbol preamble, CRC on,
        # explicit header. Worked by hand from the datasheet expression so that
        # a refactor which still self-consistently returns the wrong answer
        # fails here: the numerator is 8*28 - 4*12 + 28 + 16 = 220, the
        # denominator 4*(12 - 2) = 40 with the low-data-rate optimisation on,
        # so ceil(5.5) = 6 blocks of 5 coded symbols, plus the fixed 8.
        expected = (18 + 4.25) * 32.768 + (8 + 6 * 5) * 32.768
        self.assertAlmostEqual(hf.time_on_air_ms(12, 125.0, 28), expected, places=6)

    def test_the_low_data_rate_optimisation_switches_on_over_sixteen_ms(self):
        # Mandatory when a symbol lasts over 16 ms, which is SF11 and SF12 at
        # 125 kHz. It changes the denominator, so a frame at SF11 needs *more*
        # symbols than the SF10 arithmetic would suggest.
        self.assertLessEqual(hf.symbol_time_ms(10, 125.0), 16.0)
        self.assertGreater(hf.symbol_time_ms(11, 125.0), 16.0)
        # SF11 with the optimisation on: 8*28 - 44 + 44 = 224 over 4*(11 - 2)
        # = 36, so ceil(6.22) = 7 blocks of 5. Without it the denominator would
        # be 44 and five blocks would do — the frame is longer *because* the
        # optimisation is mandatory, which is the counter-intuitive part.
        symbols = hf.time_on_air_ms(11, 125.0, 28) / hf.symbol_time_ms(11, 125.0)
        self.assertEqual(symbols, 22.25 + 8 + 7 * 5)

    def test_airtime_rises_with_the_spreading_factor(self):
        last = 0.0
        for sf in range(7, 13):
            now = hf.time_on_air_ms(sf, 125.0, 28)
            self.assertGreater(now, last)
            last = now


class TheDutyCyclePredictionIsTheDriversOwn(unittest.TestCase):

    def test_the_sleep_is_two_symbols_at_the_shipped_preamble(self):
        # RadioLib sleeps through preamble - 2 * minSymbols, and minSymbols is 8
        # from SF7 up. An 18-symbol preamble therefore buys two symbols, not
        # eighteen — the correction that reshaped this whole feature.
        for sf in range(7, 13):
            self.assertEqual(hf.rx_duty_cycle_sleep_us(sf, 125.0),
                             2 * int((10000 << sf) / 1250.0))

    def test_a_preamble_no_longer_than_the_two_windows_sleeps_not_at_all(self):
        self.assertEqual(hf.rx_duty_cycle_sleep_us(9, 125.0, preamble_syms=16), 0)
        self.assertEqual(hf.rx_duty_cycle_sleep_us(9, 125.0, preamble_syms=6), 0)

    def test_the_shipped_channel_cannot_engage_and_sf9_upwards_can(self):
        # This is the finding that makes the original acceptance criterion
        # partly vacuous: at the shipped SF8/125 kHz the two-symbol sleep is
        # 4096 us, under the 6016 us the driver insists on, so it silently arms
        # a continuous receive. Two of the six spreading factors in the criterion
        # as written prove nothing about the feature.
        self.assertFalse(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(7, 125.0)))
        self.assertFalse(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(8, 125.0)))
        for sf in range(9, 13):
            self.assertTrue(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(sf, 125.0)),
                            "SF%d at 125 kHz should engage" % sf)

    def test_the_threshold_is_exactly_the_tcxo_ramp_plus_the_transition(self):
        edge = hf.RX_DC_TCXO_DELAY_US + hf.RX_DC_TRANSITION_US
        self.assertFalse(hf.rx_duty_cycle_engages(edge - 1))
        self.assertTrue(hf.rx_duty_cycle_engages(edge))

    def test_a_sleep_too_wide_for_the_chips_counter_is_refused_as_well(self):
        # The other half of the driver's acceptance test, and the dangerous one:
        # this refusal happens before the mode is staged, so the chip is left in
        # standby and the node is deaf. Predicting it as "engages" would send
        # this harness to score a receiver that was not listening at all.
        raw_max_us = hf.RX_DC_PERIOD_RAW_MAX * 125 // 8
        self.assertTrue(hf.rx_duty_cycle_engages(raw_max_us + 6000))
        self.assertFalse(hf.rx_duty_cycle_engages(raw_max_us + 6001 + 125))

    def test_engagement_follows_the_symbol_time_and_not_the_spreading_factor(self):
        # Why the plan narrows the bandwidth for SF7 and SF8 rather than giving
        # up on them: the sleep is a fixed two symbols, so what decides is how
        # long a symbol is. Every channel with the same symbol time behaves
        # identically, whatever SF produced it.
        same = [(8, 62.5), (9, 125.0), (10, 250.0), (11, 500.0)]
        sleeps = {hf.rx_duty_cycle_sleep_us(sf, bw) for sf, bw in same}
        self.assertEqual(len(sleeps), 1, "these channels should have one sleep period")

    def test_narrowing_is_what_lets_sf7_and_sf8_engage_at_all(self):
        self.assertTrue(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(7, 41.7)))
        self.assertTrue(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(8, 62.5)))
        self.assertFalse(hf.rx_duty_cycle_engages(hf.rx_duty_cycle_sleep_us(7, 62.5)))


# ===========================================================================
#  The transmit allowance, which is what makes a run take days or hours
# ===========================================================================

class TheSenderIsPacedByTheSubBandNotByAGuess(unittest.TestCase):

    def test_the_shipped_channel_is_in_the_ten_percent_sub_band(self):
        # The assumption worth testing out loud, because "EU 868, so 1 %" is the
        # reflex and it is wrong here by a factor of ten. 869.525 MHz with a
        # 125 kHz channel sits wholly inside 869.4-869.65, which ERC 70-03 gives
        # 10 %; the node holds 95 % of that.
        band = hf.band_for(869.525, 125.0)
        self.assertEqual(band[2], 1000)
        self.assertEqual(hf.effective_basis_points(869.525, 125.0), 950)

    def test_a_wide_channel_falls_out_of_its_sub_band_and_loses_the_allowance(self):
        # 500 kHz centred on 869.525 reaches into two unallocated ranges, and the
        # strictest allowance the channel touches is what governs. This is why
        # the plan stops at 250 kHz: a 500 kHz point would be paced a hundred
        # times slower without anybody noticing why.
        self.assertEqual(hf.effective_basis_points(869.525, 250.0), 950)
        self.assertEqual(hf.effective_basis_points(869.525, 500.0), 9)

    def test_an_allowance_is_never_rounded_away_to_nothing(self):
        # 0.1 % less the 5 % margin is 0.095 %, which truncates to 0.09 % in
        # basis points; the firmware floors it at 1 rather than 0, because a
        # zero here would read as "no limit" and mean the opposite.
        self.assertGreaterEqual(hf.effective_basis_points(863.5, 125.0), 1)

    def test_a_manual_cap_only_ever_tightens(self):
        self.assertEqual(hf.effective_basis_points(869.525, 125.0, manual_pct=1), 100)
        self.assertEqual(hf.effective_basis_points(869.525, 125.0, manual_pct=50), 950)

    def test_the_gap_takes_the_larger_of_the_legal_and_the_mechanical_floor(self):
        point = hf.Point(7, 125.0, 10)
        # At SF7 the frame is short, so the allowance dominates: 77 ms of
        # airtime at 9.5 % is about 810 ms.
        wide = point.gap_s(869.525, hf.PAYLOAD_BYTES, 5, poll_ms=0.0, settle_ms=0.0)
        self.assertAlmostEqual(wide, point.toa_ms() / 1000.0 / 0.095, places=3)
        # Give the console round trips a realistic cost and the mechanical floor
        # takes over, because a window cannot be shorter than the work in it.
        narrow = point.gap_s(869.525, hf.PAYLOAD_BYTES, 5, poll_ms=800.0, settle_ms=400.0)
        self.assertGreater(narrow, wide)

    def test_a_channel_with_no_known_allowance_is_refused_rather_than_flat_out(self):
        point = hf.Point(9, 125.0, 10)
        with self.assertRaises(ValueError):
            point.gap_s(915.0, hf.PAYLOAD_BYTES, 5)
        # ...unless the operator states one, which is the only honest way to
        # transmit thousands of frames outside a band plan this tool knows.
        self.assertGreater(point.gap_s(915.0, hf.PAYLOAD_BYTES, 5, manual_pct=1), 0)


class TheOriginalCriterionIsNotRunnable(unittest.TestCase):
    """The arithmetic that condemns "10 000 frames at SF7-SF12", kept as a test
    so that a later attempt to reinstate it has to argue with a number."""

    class _Args:
        freq_mhz = hf.RF_FREQ_MHZ
        cr = hf.RF_CR
        poll_ms = 150.0
        settle_ms = 400.0
        duty_cycle_pct = 0

    def _hours(self, plan_name, frames=500, fallback=200):
        points = hf.build_plan(plan_name, frames, fallback)
        return hf.describe_plan(points, self._Args(), out=lambda *a, **k: None) / 3600.0

    def test_ten_thousand_frames_per_spreading_factor_costs_over_a_week(self):
        hours = self._hours("original")
        self.assertGreater(hours, 200.0)

    def test_the_replacement_fits_in_a_day(self):
        self.assertLess(self._hours("claim", 500, 200), 24.0)
        self.assertLess(self._hours("claim", 300, 200), 12.0)

    def test_the_criterion_as_written_covers_two_channels_that_cannot_engage(self):
        cannot = [p for p in hf.build_plan("original", 1, 1) if not p.engages]
        self.assertEqual([(p.sf, p.bw_khz) for p in cannot], [(7, 125.0), (8, 125.0)])

    def test_the_replacement_covers_every_spreading_factor_with_the_mode_armed(self):
        engaging = {p.sf for p in hf.build_plan("claim", 1, 1) if p.engages}
        self.assertEqual(engaging, {7, 8, 9, 10, 11, 12})


# ===========================================================================
#  The step list: interleaving, and what makes a run resumable
# ===========================================================================

class TheArmsAreInterleavedAndTheOrderIsStable(unittest.TestCase):

    def test_each_arm_gets_exactly_the_frames_it_was_promised(self):
        steps = hf.steps_for(hf.Point(9, 125.0, 30), block=7)
        for arm in (hf.ARM_OFF, hf.ARM_ON):
            seqs = [s for a, s in steps if a == arm]
            self.assertEqual(seqs, list(range(30)), arm)

    def test_the_two_arms_alternate_so_neither_owns_a_quiet_hour(self):
        # Both arms have to run through the same minutes. An hour of drift on an
        # over-the-air link is larger than the effect being measured, so a
        # sequential off-then-on sweep would attribute the weather to the
        # feature.
        steps = hf.steps_for(hf.Point(9, 125.0, 20), block=5)
        arms = [a for a, _ in steps]
        self.assertEqual(arms[:5], [hf.ARM_OFF] * 5)
        self.assertEqual(arms[5:10], [hf.ARM_ON] * 5)
        # ...and the order within a block flips, so a drift *inside* a block
        # cannot favour whichever arm always went first.
        self.assertEqual(arms[10:15], [hf.ARM_ON] * 5)
        self.assertEqual(arms[15:20], [hf.ARM_OFF] * 5)

    def test_a_partial_last_block_does_not_overrun_the_frame_count(self):
        steps = hf.steps_for(hf.Point(9, 125.0, 12), block=5)
        self.assertEqual(len(steps), 24)

    def test_the_list_is_deterministic_which_is_what_resume_relies_on(self):
        a = hf.steps_for(hf.Point(11, 125.0, 40), block=9)
        b = hf.steps_for(hf.Point(11, 125.0, 40), block=9)
        self.assertEqual(a, b)

    def test_a_block_of_nothing_is_refused_rather_than_looping_for_ever(self):
        with self.assertRaises(ValueError):
            hf.steps_for(hf.Point(9, 125.0, 10), block=0)


class SelectingChannelsFromAPlan(unittest.TestCase):

    def test_a_spreading_factor_takes_every_bandwidth_at_it(self):
        chosen = hf.select_points(hf.build_plan("claim", 1, 1), ["sf7"])
        self.assertEqual([p.bw_khz for p in chosen], [41.7, 125.0])

    def test_a_full_name_takes_one_channel(self):
        chosen = hf.select_points(hf.build_plan("claim", 1, 1), ["sf7@41.7"])
        self.assertEqual([(p.sf, p.bw_khz) for p in chosen], [(7, 41.7)])

    def test_a_name_that_matches_nothing_selects_nothing(self):
        # Rather than quietly running the whole plan, which is how an operator
        # ends up with a nine-day sweep they did not ask for.
        self.assertEqual(hf.select_points(hf.build_plan("claim", 1, 1), ["sf13"]), [])


# ===========================================================================
#  Loss accounting
# ===========================================================================

class TheLedgerCountsWhatWasActuallyAsked(unittest.TestCase):

    def _ledger(self, *rows):
        ledger = hf.Ledger(None)
        for arm, seq, result in rows:
            ledger.record("sf9_bw125", arm, seq, result)
        return ledger

    def test_a_miss_is_identified_by_sequence_number_and_not_merely_counted(self):
        # The whole reason each frame carries a sequence and each window is
        # polled: "we lost four" is a number, "we lost 17, 18, 19 and 20" is a
        # burst and points at something that was happening at the time.
        ledger = self._ledger((hf.ARM_ON, 0, hf.RESULT_OK),
                              (hf.ARM_ON, 1, hf.RESULT_MISS),
                              (hf.ARM_ON, 2, hf.RESULT_OK),
                              (hf.ARM_ON, 3, hf.RESULT_MISS))
        tally = ledger.tally("sf9_bw125", hf.ARM_ON)
        self.assertEqual(tally["missed_seqs"], [1, 3])
        self.assertEqual((tally["sent"], tally["received"], tally["missed"]), (4, 2, 2))

    def test_a_window_the_node_transmitted_through_is_excluded_from_both_sides(self):
        # It was deaf for part of the window, so the question was not put.
        # Counting it as a loss would blame the feature for an announce;
        # counting it as a delivery would hide a real one.
        ledger = self._ledger((hf.ARM_ON, 0, hf.RESULT_OK),
                              (hf.ARM_ON, 1, hf.RESULT_NODE_TX),
                              (hf.ARM_ON, 2, hf.RESULT_NO_TX))
        tally = ledger.tally("sf9_bw125", hf.ARM_ON)
        self.assertEqual(tally["sent"], 1)
        self.assertEqual(tally["missed"], 0)
        self.assertEqual(tally["attempted"], 3)
        self.assertEqual(tally["excluded"],
                         {hf.RESULT_NO_TX: 1, hf.RESULT_NODE_TX: 1})

    def test_a_counter_that_moved_by_more_than_one_is_flagged_not_swallowed(self):
        ledger = self._ledger((hf.ARM_ON, 0, hf.RESULT_EXTRA))
        tally = ledger.tally("sf9_bw125", hf.ARM_ON)
        self.assertEqual(tally["extra_seqs"], [0])
        self.assertEqual(tally["sent"], 1)

    def test_the_arms_and_channels_are_tallied_apart(self):
        ledger = self._ledger((hf.ARM_ON, 0, hf.RESULT_MISS),
                              (hf.ARM_OFF, 0, hf.RESULT_OK))
        self.assertEqual(ledger.tally("sf9_bw125", hf.ARM_ON)["missed"], 1)
        self.assertEqual(ledger.tally("sf9_bw125", hf.ARM_OFF)["missed"], 0)
        self.assertEqual(ledger.tally("sf12_bw125", hf.ARM_ON)["sent"], 0)


class ARunSurvivesBeingInterrupted(unittest.TestCase):

    def test_frames_are_on_disk_as_they_happen_and_reload(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "run.jsonl")
            first = hf.Ledger(path)
            first.open({"plan": "claim", "payload_bytes": hf.PAYLOAD_BYTES})
            first.record("sf9_bw125", hf.ARM_OFF, 0, hf.RESULT_OK)
            first.record("sf9_bw125", hf.ARM_OFF, 1, hf.RESULT_MISS)
            first.close()

            second = hf.Ledger(path)
            header = second.load()
            self.assertEqual(header["plan"], "claim")
            self.assertEqual(second.done("sf9_bw125", hf.ARM_OFF), {0, 1})
            self.assertEqual(second.tally("sf9_bw125", hf.ARM_OFF)["missed_seqs"], [1])

    def test_a_half_written_last_line_costs_one_frame_and_not_the_run(self):
        # What a kill in the middle of a write leaves. Hours of frames must not
        # be thrown away because the last one is a fragment.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "run.jsonl")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(json.dumps({"kind": "header", "plan": "claim"}) + "\n")
                fh.write(json.dumps({"kind": "frame", "point": "sf9_bw125",
                                     "arm": "off", "seq": 0,
                                     "result": hf.RESULT_OK}) + "\n")
                fh.write('{"kind": "frame", "point": "sf9_b')
            ledger = hf.Ledger(path)
            self.assertIsNotNone(ledger.load())
            self.assertEqual(ledger.done("sf9_bw125", hf.ARM_OFF), {0})

    def test_a_second_run_appends_to_an_existing_file_without_a_second_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "run.jsonl")
            a = hf.Ledger(path)
            a.open({"plan": "claim"})
            a.record("sf9_bw125", hf.ARM_OFF, 0, hf.RESULT_OK)
            a.close()
            b = hf.Ledger(path)
            b.load()
            b.open({"plan": "claim"})
            b.record("sf9_bw125", hf.ARM_OFF, 1, hf.RESULT_OK)
            b.close()
            with open(path, encoding="utf-8") as fh:
                kinds = [json.loads(line)["kind"] for line in fh if line.strip()]
            self.assertEqual(kinds.count("header"), 1)
            # Two frames on disk, not three: reloading the first run's records
            # into memory must not write them out a second time.
            self.assertEqual(kinds.count("frame"), 2)


# ===========================================================================
#  One transmit window
# ===========================================================================

class _FakeNode:
    """A node whose counters move on a script. `polls` records every read so a
    test can assert the grace re-poll happened, or did not.

    Readings are (rx, tx) or (rx, tx, armed); the two-tuple form means armed.
    """

    def __init__(self, readings):
        self.readings = list(readings)
        self.polls = 0

    def sample(self):
        self.polls += 1
        row = self.readings.pop(0) if self.readings else (0, 0)
        return row if len(row) == 3 else (row[0], row[1], True)


class _FakeSender:
    def __init__(self, ok=True):
        self.ok = ok
        self.sent = []

    def send(self, seq, confirm_s=5.0):
        self.sent.append(seq)
        return self.ok


class OneWindowIsScoredOnWhatTheCounterDid(unittest.TestCase):

    def _run(self, readings, sender=None, grace=100.0):
        node = _FakeNode(readings)
        return node, hf.run_frame(node, sender or _FakeSender(), 0,
                                  toa_ms=0.0, settle_ms=0.0, grace_ms=grace)

    def test_the_counter_moving_by_one_is_a_delivery(self):
        _, (result, facts) = self._run([(10, 3), (11, 3)])
        self.assertEqual(result, hf.RESULT_OK)
        self.assertEqual((facts["rx_delta"], facts["tx_delta"]), (1, 0))

    def test_a_counter_that_never_moves_is_a_miss(self):
        node, (result, _) = self._run([(10, 3), (10, 3), (10, 3)])
        self.assertEqual(result, hf.RESULT_MISS)
        self.assertEqual(node.polls, 3, "a miss must be confirmed by a second look")

    def test_a_late_frame_is_rescued_by_the_grace_poll_not_scored_as_lost(self):
        # The RNode does its own carrier sense before transmitting, so the time
        # from send() to the air is not fixed. Without this second look the run
        # would report the sender's scheduler as the receiver's loss rate.
        _, (result, facts) = self._run([(10, 3), (10, 3), (11, 3)])
        self.assertEqual((result, facts["rx_delta"]), (hf.RESULT_OK, 1))

    def test_a_delivered_frame_costs_no_grace_poll(self):
        node, _ = self._run([(10, 3), (11, 3)])
        self.assertEqual(node.polls, 2)

    def test_the_node_transmitting_in_the_window_excludes_it(self):
        _, (result, facts) = self._run([(10, 3), (11, 4)])
        self.assertEqual((result, facts["tx_delta"]), (hf.RESULT_NODE_TX, 1))

    def test_a_node_transmission_excludes_the_window_even_when_nothing_arrived(self):
        # The dangerous direction: the node was deaf, so this is not evidence of
        # a miss, and scoring it as one would blame the feature for an announce.
        _, (result, _) = self._run([(10, 3), (10, 4), (10, 4)])
        self.assertEqual(result, hf.RESULT_NODE_TX)

    def test_a_frame_the_sender_never_transmitted_is_excluded_and_not_a_miss(self):
        node, (result, _) = self._run([(10, 3)], sender=_FakeSender(ok=False))
        self.assertEqual(result, hf.RESULT_NO_TX)
        self.assertEqual(node.polls, 1, "there is nothing to wait for")

    def test_two_frames_arriving_in_one_window_means_somebody_else_is_talking(self):
        _, (result, facts) = self._run([(10, 3), (12, 3)])
        self.assertEqual((result, facts["rx_delta"]), (hf.RESULT_EXTRA, 2))

    def test_a_window_that_was_not_armed_throughout_is_recorded_as_unarmed(self):
        # The receiver drops out of the duty-cycled mode on every reception and
        # every transmission, and it is re-armed afterwards. A frame caught
        # while it was between modes is not evidence about a sleeping receiver,
        # so the optimistic half of the pair must not be the one recorded.
        _, (_, facts) = self._run([(10, 3, True), (11, 3, False)])
        self.assertFalse(facts["armed"])
        _, (_, facts) = self._run([(10, 3, False), (11, 3, True)])
        self.assertFalse(facts["armed"])
        _, (_, facts) = self._run([(10, 3, True), (11, 3, True)])
        self.assertTrue(facts["armed"])


# ===========================================================================
#  Reading the node
# ===========================================================================

class _FakeConsole:
    def __init__(self, replies):
        self.replies = replies
        self.commands = []

    def command(self, line):
        self.commands.append(line)
        reply = self.replies.get(line.split()[0].upper())
        if reply is None:
            return "ERR", {"text": "no such command"}, []
        return "OK", {}, reply

    def close(self):
        pass


class ReadingTheNodeIsGatedOnWhatItSaid(unittest.TestCase):

    STATUS = [
        {"uptime_s": "600", "boot_count": "7"},
        {"radio": "online", "model": "SX1262", "rx": "4211", "tx": "97",
         "cad_timeouts": "0", "cad_arm_errors": "0"},
        {"rx_duty_cycle": "on", "supported": "yes", "sleep_us": "8192",
         "engages": "yes", "armed": "yes"},
    ]

    def test_every_status_line_is_folded_into_one_reading(self):
        node = hf.Node(_FakeConsole({"STATUS": self.STATUS}))
        status = node.status()
        self.assertEqual(status["rx"], "4211")
        self.assertEqual(status["armed"], "yes")
        self.assertEqual(status["model"], "SX1262")

    def test_the_counters_come_back_as_numbers_beside_the_armed_flag(self):
        node = hf.Node(_FakeConsole({"STATUS": self.STATUS}))
        self.assertEqual(node.sample(), (4211, 97, True))
        self.assertEqual(node.counters(), (4211, 97))

    def test_armed_is_false_unless_the_node_actually_said_yes(self):
        # Never inferred from the setting or from `engages`. RadioLib returns
        # success for a duty-cycle call it silently turned into a continuous
        # receive, so this field is the only outside view of which mode the
        # chip is really in.
        status = [dict(self.STATUS[1]),
                  dict(self.STATUS[2], armed="no", rx_duty_cycle="on", engages="yes")]
        node = hf.Node(_FakeConsole({"STATUS": status}))
        self.assertEqual(node.sample()[2], False)

    def test_a_status_with_no_receive_counter_raises_rather_than_reading_zero(self):
        # A default of 0 here would turn a firmware that stopped publishing the
        # counter into a run in which every single frame was missed — a result
        # that looks like a damning finding and is a parsing bug.
        node = hf.Node(_FakeConsole({"STATUS": [{"uptime_s": "600"}]}))
        with self.assertRaises(RuntimeError):
            node.sample()

    def test_a_refused_command_is_an_exception_and_not_an_empty_list(self):
        node = hf.Node(_FakeConsole({}))
        with self.assertRaises(RuntimeError):
            node.status()

    def test_a_setting_is_read_back_under_the_name_the_console_gives_it(self):
        # The console's key/value reader takes \w+ for a key, which stops at the
        # dot: "RM GET radio.sf=8" arrives as sf=8, not radio.sf=8.
        node = hf.Node(_FakeConsole({"GET": [{"sf": "9"}]}))
        self.assertEqual(node.get("radio.sf"), "9")


# ===========================================================================
#  Statistics, and the verdict they support
# ===========================================================================

class WhatNFramesActuallyProve(unittest.TestCase):

    def test_no_misses_in_n_bounds_the_rate_at_three_over_n(self):
        self.assertAlmostEqual(hf.rule_of_three_upper(300), 0.009986, places=5)
        self.assertAlmostEqual(hf.rule_of_three_upper(500), 0.005991, places=5)
        # The number that condemns the original criterion from the other side:
        # ten thousand frames buys a bound of 0.03 %, which is far below the
        # loss floor of any real over-the-air link, so the extra frames buy
        # precision the measurement cannot deliver.
        self.assertLess(hf.rule_of_three_upper(10000), 0.0004)

    def test_the_wilson_bound_does_not_collapse_to_zero_at_zero_misses(self):
        # The textbook normal interval has zero width when k is 0 and would
        # report a link proven perfect by four frames.
        self.assertGreater(hf.wilson_upper(0, 4), 0.3)
        self.assertLess(hf.wilson_upper(0, 500), 0.008)
        self.assertGreater(hf.wilson_upper(0, 500), 0.0)

    def test_the_bound_rises_with_the_misses_and_falls_with_the_frames(self):
        self.assertGreater(hf.wilson_upper(5, 500), hf.wilson_upper(1, 500))
        self.assertGreater(hf.wilson_upper(1, 100), hf.wilson_upper(1, 1000))

    def test_the_rule_of_three_and_wilson_agree_about_zero_misses(self):
        for n in (100, 300, 500, 1000):
            self.assertAlmostEqual(hf.wilson_upper(0, n), hf.rule_of_three_upper(n),
                                   delta=0.15 * hf.rule_of_three_upper(n))


class TheExcessLossTestIsExact(unittest.TestCase):

    def test_equal_losses_are_not_evidence_of_anything(self):
        self.assertGreater(hf.excess_loss_pvalue(3, 500, 3, 500), 0.3)

    def test_no_losses_at_all_cannot_be_significant(self):
        self.assertEqual(hf.excess_loss_pvalue(0, 500, 0, 500), 1.0)

    def test_every_loss_landing_in_the_duty_cycled_arm_is(self):
        # Ten misses on one side and none on the other. Close to a coin landing
        # heads ten times, and deliberately not equal to it: the exact test
        # draws without replacement from the two arms, so it is slightly
        # *stricter* than 2^-10 while the arms are finite.
        p = hf.excess_loss_pvalue(10, 500, 0, 500)
        self.assertLess(p, 0.5 ** 10)
        self.assertGreater(p, 0.5 ** 11)

    def test_losses_in_the_control_arm_are_never_held_against_the_feature(self):
        # One-sided, deliberately: the question is whether the duty cycle costs
        # frames, and a control arm that lost more is not a finding about it.
        self.assertEqual(hf.excess_loss_pvalue(0, 500, 10, 500), 1.0)

    def test_uneven_arms_are_handled_rather_than_assumed_away(self):
        # An interrupted run leaves the arms uneven, and the same three misses
        # out of a smaller sample are more surprising, not less.
        big = hf.excess_loss_pvalue(3, 500, 0, 500)
        small = hf.excess_loss_pvalue(3, 100, 0, 500)
        self.assertLess(small, big)


class TheVerdictSaysWhichGateFailed(unittest.TestCase):

    def _tally(self, missed, sent):
        return {"missed": missed, "sent": sent, "received": sent - missed,
                "missed_seqs": list(range(missed)), "extra_seqs": [],
                "attempted": sent, "excluded": {}}

    def test_a_clean_pair_of_arms_passes(self):
        ok, why = hf.verdict(self._tally(0, 500), self._tally(0, 500),
                             armed_on=True, armed_off=False, engages=True)
        self.assertTrue(ok, why)

    def test_a_receiver_that_never_armed_is_not_evidence_about_a_sleeping_one(self):
        # The failure this harness exists to prevent. RadioLib returns success
        # for a duty-cycle call it silently turned into a continuous receive, so
        # a run that did not check `armed=` would collect a perfect result about
        # the wrong mode.
        ok, why = hf.verdict(self._tally(0, 500), self._tally(0, 500),
                             armed_on=False, armed_off=False, engages=True)
        self.assertFalse(ok)
        self.assertIn("never armed", why)

    def test_a_control_arm_that_armed_the_mode_invalidates_the_comparison(self):
        ok, why = hf.verdict(self._tally(0, 500), self._tally(0, 500),
                             armed_on=True, armed_off=True, engages=True)
        self.assertFalse(ok)

    def test_a_fallback_channel_passes_only_while_it_stays_a_fallback(self):
        ok, _ = hf.verdict(self._tally(0, 200), self._tally(0, 200),
                           armed_on=False, armed_off=False, engages=False)
        self.assertTrue(ok)
        ok, why = hf.verdict(self._tally(0, 200), self._tally(0, 200),
                             armed_on=True, armed_off=False, engages=False)
        self.assertFalse(ok)
        self.assertIn("predicted", why)

    def test_a_significant_excess_in_the_duty_cycled_arm_fails(self):
        ok, why = hf.verdict(self._tally(9, 500), self._tally(0, 500),
                             armed_on=True, armed_off=False, engages=True)
        self.assertFalse(ok)
        self.assertIn("p=", why)

    def test_a_rig_that_loses_heavily_in_both_arms_still_fails(self):
        # The gate the significance test alone would miss: two arms that agree
        # about losing a quarter of the traffic prove they are alike and nothing
        # whatever about whether the feature is safe to arm.
        ok, why = hf.verdict(self._tally(120, 500), self._tally(120, 500),
                             armed_on=True, armed_off=False, engages=True)
        self.assertFalse(ok)
        self.assertIn("upper bound", why)

    def test_an_arm_with_nothing_scored_is_not_a_pass(self):
        # Every window excluded — the node transmitting throughout, or the
        # sender never getting a frame away — leaves no evidence, and a
        # zero-over-zero loss rate must not read as a clean sweep.
        ok, why = hf.verdict(self._tally(0, 0), self._tally(0, 500),
                             armed_on=True, armed_off=False, engages=True)
        self.assertFalse(ok)
        self.assertIn("nothing was scored", why)


# ===========================================================================
#  The frames themselves
# ===========================================================================

class TheFrameCarriesItsOwnSequence(unittest.TestCase):

    def test_the_payload_is_a_magic_and_a_sequence_number(self):
        self.assertEqual(hf.frame_payload(0), b"RMHF" + b"\x00\x00\x00\x00")
        self.assertEqual(hf.frame_payload(499)[4:], struct.pack("!I", 499))

    def test_every_frame_in_a_run_is_the_same_length(self):
        # The airtime, the pacing and the frame the receiver has to catch all
        # follow the length, so a payload that grew with the sequence number
        # would make the last frames a different experiment from the first.
        lengths = {len(hf.frame_payload(s)) for s in (0, 1, 255, 256, 65535, 10 ** 6)}
        self.assertEqual(lengths, {hf.PAYLOAD_BYTES})

    def test_what_goes_on_the_air_is_the_payload_plus_both_headers(self):
        self.assertEqual(hf.on_air_bytes(8), 8 + 19 + 1)

    @unittest.skipUnless(importlib.util.find_spec("RNS"), "RNS is not importable here")
    def test_the_rns_header_length_is_what_reticulum_actually_packs(self):
        # Only runs where RNS is installed — the venv this harness is driven
        # from. Elsewhere the constant stands on the comment beside it.
        import RNS
        self.assertEqual(hf.RNS_HEADER_BYTES,
                         2 + RNS.Reticulum.TRUNCATED_HASHLENGTH // 8 + 1)


class TheSendersConfigNamesTheChannelUnderTest(unittest.TestCase):

    def _sender(self, mode="owned"):
        return hf.Sender(mode, "/nonexistent", "/dev/ttyACM1", 869.525, 7, 5,
                         python="/usr/bin/python3")

    def test_the_written_config_carries_the_channel_the_plan_asked_for(self):
        # Reticulum has no live retune for an RNodeInterface, so a channel
        # change is a config rewrite and a fresh instance. If this ever silently
        # kept the old channel the sweep would transmit six identical runs and
        # report six passes.
        text = self._sender().config_text(11, 250.0)
        self.assertIn("spreadingfactor = 11", text)
        self.assertIn("bandwidth = 250000", text)
        self.assertIn("frequency = 869525000", text)
        self.assertIn("port = /dev/ttyACM1", text)
        self.assertIn("share_instance = No", text)

    def test_a_fractional_bandwidth_survives_the_trip_to_hertz(self):
        self.assertIn("bandwidth = 41700", self._sender().config_text(7, 41.7))
        self.assertIn("bandwidth = 62500", self._sender().config_text(8, 62.5))

    def test_the_child_is_launched_under_the_interpreter_that_has_rns(self):
        # The two halves of this harness need different interpreters on this
        # bench: the parent needs pyserial for the console, the child needs RNS.
        argv = self._sender().worker_argv()
        self.assertEqual(argv[0], "/usr/bin/python3")
        self.assertIn("--send-worker", argv)
        self.assertIn("--rns-config", argv)

    def test_the_shared_mode_child_is_told_so(self):
        self.assertIn("shared", self._sender("shared").worker_argv())


class _StubWorker:
    """A child that speaks the worker protocol and needs neither RNS nor a radio.

    The plumbing between the two processes is worth a real test rather than a
    mock: a pipe that deadlocks, a line that is never flushed or a QUIT that is
    not acted on are all failures that only appear when there really are two
    processes, and all three would present on the bench as a sweep that stalled
    hours in with no output.
    """

    SCRIPT = (
        "import sys\n"
        "print('READY confirmed', flush=True)\n"
        "while True:\n"
        "    line = sys.stdin.readline()\n"
        "    if not line or line.strip() == 'QUIT': break\n"
        "    seq = int(line.split()[1])\n"
        "    print(('FAIL %d 0' if seq == 7 else 'SENT %d 28') % seq, flush=True)\n")

    def __init__(self, tmp):
        self.path = os.path.join(tmp, "stub_worker.py")
        with open(self.path, "w", encoding="utf-8") as fh:
            fh.write(self.SCRIPT)


class TheSenderRunsAsARealChildProcess(unittest.TestCase):

    def _sender(self, tmp):
        sender = hf.Sender("owned", os.path.join(tmp, "cfg"), "/dev/null",
                           869.525, 7, 5, python=sys.executable)
        stub = _StubWorker(tmp)
        sender.worker_argv = lambda: [sys.executable, stub.path]
        return sender

    def test_a_channel_comes_up_transmits_and_shuts_down(self):
        with tempfile.TemporaryDirectory() as tmp:
            sender = self._sender(tmp)
            try:
                self.assertTrue(sender.tune(9, 125.0).startswith("READY"))
                self.assertTrue(sender.confirms)
                self.assertTrue(sender.send(0))
                self.assertTrue(sender.send(1))
                # The one the stub refuses: a frame the radio never got is a
                # window to exclude, not a receiver that missed.
                self.assertFalse(sender.send(7))
            finally:
                sender.teardown()
            self.assertIsNone(sender.proc)

    def test_the_channels_config_is_on_disk_beside_the_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            sender = self._sender(tmp)
            try:
                sender.tune(12, 250.0)
            finally:
                sender.teardown()
            with open(os.path.join(tmp, "cfg", "config"), encoding="utf-8") as fh:
                written = fh.read()
            self.assertIn("spreadingfactor = 12", written)
            self.assertIn("bandwidth = 250000", written)

    def test_a_child_that_says_nothing_is_a_timeout_and_not_a_hang(self):
        # The failure a timed readline() loop cannot catch, because readline()
        # blocks until the pipe closes: a child that came up and then wedged.
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "silent.py")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("import time\ntime.sleep(60)\n")
            sender = hf.Sender("owned", os.path.join(tmp, "cfg"), "/dev/null",
                               869.525, 7, 5, python=sys.executable)
            sender.worker_argv = lambda: [sys.executable, path]
            with self.assertRaises(RuntimeError):
                sender.tune(9, 125.0, ready_s=0.5)
            self.assertIsNone(sender.proc)

    def test_a_child_that_dies_at_once_is_reported_not_waited_on(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "dead.py")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("import sys\nprint('ERR no such port', flush=True)\n")
            sender = hf.Sender("owned", os.path.join(tmp, "cfg"), "/dev/null",
                               869.525, 7, 5, python=sys.executable)
            sender.worker_argv = lambda: [sys.executable, path]
            with self.assertRaises(RuntimeError) as caught:
                sender.tune(9, 125.0, ready_s=10.0)
            self.assertIn("no such port", str(caught.exception))


class TheParentBelievesOnlyWhatTheChildSaid(unittest.TestCase):

    def test_a_matching_confirmation_is_a_transmission(self):
        self.assertTrue(hf.parse_worker_reply("SENT 41 28", 41))

    def test_a_confirmation_for_another_frame_is_not(self):
        # If the two sides lose step, treating a stale reply as this frame's
        # success attributes one frame's fate to another's window for the rest
        # of the channel — a shifted result that still looks orderly.
        self.assertFalse(hf.parse_worker_reply("SENT 40 28", 41))

    def test_silence_and_failure_are_both_a_no(self):
        for line in (None, "", "FAIL 41 0", "ERR the port went away", "READY confirmed"):
            self.assertFalse(hf.parse_worker_reply(line, 41), repr(line))


if __name__ == "__main__":
    unittest.main()
