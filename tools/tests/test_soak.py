# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
#
# soak.py's reading of a node, and its reading of a CSV.
#
# One rule is worth a test file of its own: **blank is not zero**. A node whose
# RTC domain dropped reports no previous-run figures at all, and a node whose
# previous run was clean reports zeros. Collapsing those two is not a cosmetic
# bug — it is the bug this whole column exists to fix. A Wireless Bridge
# panicked seven times in a week while every reading said "0 allocation
# failures", because every reading was taken after the reboot that zeroed the
# counter, and "0" was believed. One `.get(key, 0)` where the code says
# `.get(key, "")` puts that back, silently, and nothing else in the repository
# would notice.

import contextlib
import csv
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

_HERE = os.path.dirname(os.path.abspath(__file__))
_SOAK = os.path.join(_HERE, os.pardir, "soak.py")

_spec = importlib.util.spec_from_file_location("soak", _SOAK)
soak = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(soak)


def _status(**boot):
    """An /api/status body with `boot` overridden, everything else plausible."""
    body = {
        "uptime_s": 600,
        "diag": {
            "boot": {"count": 7, "reason": 3, "reason_name": "panic or unhandled exception",
                     "clean": False, **boot},
            "heap": {"free": 50000, "min_free": 40000, "largest_block": 30000,
                     "dram_free": 40000, "dram_min_free": 30000, "dram_largest_block": 20000},
            "faults": {"alloc_failures": 0, "contained": 0},
            "stacks": {"loopTask": 3000, "radio": 2000, "rns": 6000},
            "stack_lowest": 2000, "stack_lowest_task": "radio",
            "tables": {"paths": 3, "links": 0, "destinations": 2, "announces": 0},
        },
        "radio": {}, "transport": {"online": True}, "neighbors": [], "airtime": {},
    }
    return body


@contextlib.contextmanager
def _mocked_node(body):
    payload = json.dumps(body).encode()

    class _Resp(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    with mock.patch.object(soak.urllib.request, "urlopen", return_value=_Resp(payload)):
        yield


class SampleReadsThePreviousRun(unittest.TestCase):
    def test_a_dropped_rtc_domain_leaves_the_columns_blank_not_zero(self):
        # The firmware *omits* these keys when the RTC domain did not hold —
        # WifiManager only writes them under `if (b.prevKnown)`. The mock has to
        # omit them too: a JSON null is a different shape, and testing against
        # the shape the node never sends would let a regression through on the
        # one it does. Recording 0 here reports a power cut as a clean run.
        with _mocked_node(_status()):
            row = soak.sample("node")
        self.assertEqual(row["reachable"], 1)
        self.assertNotIn("prev_uptime_s", _status()["diag"]["boot"])   # the premise
        self.assertEqual(row["prev_alloc_failures"], "")
        self.assertEqual(row["prev_contained"], "")
        self.assertNotEqual(row["prev_alloc_failures"], 0)

    def test_an_explicit_null_is_not_recorded_as_zero_either(self):
        # Not a shape this firmware emits, but the CSV must not turn one into a
        # clean run if a future build or a proxy ever produces it.
        with _mocked_node(_status(prev_uptime_s=None,
                                  prev_alloc_failures=None, prev_contained=None)):
            row = soak.sample("node")
        for k in ("prev_uptime_s", "prev_alloc_failures", "prev_contained"):
            self.assertIn(row[k], ("", None), f"{k} was {row[k]!r}")
            self.assertNotEqual(row[k], 0)

    def test_the_figures_are_recorded_when_the_firmware_reports_them(self):
        with _mocked_node(_status(prev_uptime_s=15132,
                                  prev_alloc_failures=23, prev_contained=8)):
            row = soak.sample("node")
        self.assertEqual(row["prev_alloc_failures"], 23)
        self.assertEqual(row["prev_contained"], 8)
        self.assertEqual(row["prev_uptime_s"], 15132)

    def test_a_clean_previous_run_records_a_real_zero(self):
        with _mocked_node(_status(prev_uptime_s=86400,
                                  prev_alloc_failures=0, prev_contained=0)):
            row = soak.sample("node")
        self.assertEqual(row["prev_alloc_failures"], 0)
        self.assertNotEqual(row["prev_alloc_failures"], "")

    def test_an_unreachable_node_records_no_figures_at_all(self):
        with mock.patch.object(soak.urllib.request, "urlopen",
                               side_effect=OSError("no route")):
            row = soak.sample("node")
        self.assertEqual(row["reachable"], 0)
        self.assertEqual(row["prev_alloc_failures"], "")


class SummariseNamesTheDeadRun(unittest.TestCase):
    def _summarise(self, rows):
        fd, path = tempfile.mkstemp(suffix=".csv")
        os.close(fd)
        try:
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=soak.FIELDS)
                w.writeheader()
                for r in rows:
                    full = {k: "" for k in soak.FIELDS}
                    full.update(r)
                    w.writerow(full)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                soak.summarise(path)
            return out.getvalue()
        finally:
            os.unlink(path)

    def _rows(self, prev_alloc, prev_contained, prev_uptime=15132):
        base = dict(node="n", reachable=1, uptime_s=600, boot_reason="panic or unhandled exception",
                    heap_free=50000, heap_min=40000, heap_largest=30000,
                    stack_lowest=2000, stack_lowest_task="rns")
        return [
            dict(base, ts="2026-09-08T00:00:00+00:00", boot_count=5),
            dict(base, ts="2026-09-08T01:00:00+00:00", boot_count=6,
                 prev_uptime_s=prev_uptime,
                 prev_alloc_failures=prev_alloc, prev_contained=prev_contained),
        ]

    def test_a_run_that_died_short_of_memory_is_named(self):
        out = self._summarise(self._rows(9, 3))
        self.assertIn("9 allocation failure(s)", out)
        self.assertIn("died short of memory", out)
        self.assertIn("contained 3 exception(s)", out)

    def test_the_dead_run_is_reported_under_its_own_restart(self):
        # One restart is one event. The reason and the explanation belong in
        # the same block, not in separate passes with a range summary between.
        out = self._summarise(self._rows(9, 3))
        lines = [l for l in out.splitlines() if l.strip()]
        reason = next(i for i, l in enumerate(lines) if "reason=" in l)
        detail = next(i for i, l in enumerate(lines) if "died short of memory" in l)
        self.assertEqual(detail, reason + 1)
        # ...and the restart line itself still carries the run length.
        self.assertIn("previous run 15132s", lines[reason])

    def test_contained_exceptions_are_not_called_a_memory_shortage(self):
        # `contained` counts every exception guard() caught; a contained socket
        # failure says nothing about memory. Claiming otherwise puts a false
        # "short of memory" on a healthy node.
        out = self._summarise(self._rows(0, 4))
        self.assertIn("contained 4 exception(s)", out)
        self.assertNotIn("died short of memory", out)

    def test_a_clean_previous_run_is_reported_as_clean(self):
        out = self._summarise(self._rows(0, 0))
        self.assertIn("reported no allocation failures", out)
        self.assertNotIn("died short of memory", out)

    def test_a_power_cut_says_nothing_rather_than_reporting_a_clean_run(self):
        # The whole point. Blank columns mean the RTC domain dropped and there
        # is nothing to report; claiming the run was clean would clear a node
        # the evidence never cleared.
        out = self._summarise(self._rows("", "", prev_uptime=""))
        self.assertNotIn("died short of memory", out)
        self.assertNotIn("reported no allocation failures", out)
        self.assertIn("unknown (power lost)", out)

    def test_a_csv_written_before_the_columns_existed_is_silent(self):
        # Old files must summarise exactly as they always did.
        rows = self._rows("", "", prev_uptime="")
        legacy = soak.FIELDS[:-2]
        fd, path = tempfile.mkstemp(suffix=".csv")
        os.close(fd)
        try:
            with open(path, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=legacy)
                w.writeheader()
                for r in rows:
                    w.writerow({k: r.get(k, "") for k in legacy})
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                soak.summarise(path)
            text = out.getvalue()
        finally:
            os.unlink(path)
        self.assertNotIn("died short of memory", text)
        self.assertNotIn("reported no allocation failures", text)
        self.assertIn("RESTARTED during the run", text)

    def test_columns_are_only_ever_appended(self):
        # The file's own rule: appending anywhere but the end moves every column
        # after it out from under a CSV written before the change.
        #
        # Pinned against a checked-in fixture, not against the names of the last
        # two columns (that assertion had to be edited every time the rule was
        # *obeyed*) and not against a file under roadmap/, which is git-excluded
        # — that version passed here and skipped in CI, so the rule guarding
        # every historical CSV was enforced on one machine only.
        #
        # When you append a column, append it to the fixture too. That is the
        # point: the edit is the record of what the header used to be.
        import csv as _csv, os as _os
        fx = _os.path.join(_os.path.dirname(__file__), "fixtures", "soak-header.csv")
        with open(fx, newline="") as f:
            frozen = next(_csv.reader(f))
        self.assertEqual(soak.FIELDS[:len(frozen)], frozen,
                         "a column was inserted or renamed, not appended")

class DischargeIsTheOnlyPowerMeasurement(unittest.TestCase):
    """No board on this bench can report its own current, so the rate the cell
    falls at is the whole measurement. These pin the three ways it can be
    invalid, because a number reported from an invalid run is worse than no
    number: it would be used."""

    def _rows(self, samples, profile="battery", charging=0, present=1, boot=5):
        # samples: (hours_from_start, volts)
        out = []
        for h, v in samples:
            out.append(dict(
                ts=f"2026-09-08T{int(h):02d}:{int((h % 1) * 60):02d}:00+00:00",
                node="n", reachable=1, uptime_s=int(h * 3600), boot_count=boot,
                boot_reason="power-on", heap_free=50000, heap_min=40000,
                heap_largest=30000, stack_lowest=2000, stack_lowest_task="rns",
                power_profile=profile, battery_v=v, battery_present=present,
                battery_charging=charging))
        return out

    def test_a_falling_cell_reports_a_rate(self):
        out = soak.discharge(self._rows([(0, 4.100), (4, 3.900)]))
        text = "\n".join(out)
        self.assertIn("4.100 V -> 3.900 V", text)
        self.assertIn("profile battery", text)
        self.assertIn("fell 200 mV in 4.00 h (50.0 mV/h)", text)

    def test_a_charging_node_is_not_a_discharge_measurement(self):
        # The guard that matters most. A node on USB has a rising voltage that
        # says something about the charger and nothing about the firmware.
        out = soak.discharge(self._rows([(0, 3.900), (4, 4.100)], charging=1))
        text = "\n".join(out)
        self.assertIn("was charging", text)
        self.assertNotIn("mV/h", text)

    def test_a_profile_change_invalidates_the_run(self):
        rows = self._rows([(0, 4.100), (2, 4.000)], profile="battery")
        rows += self._rows([(4, 3.900)], profile="performance")
        text = "\n".join(soak.discharge(rows))
        self.assertIn("power profile changed", text)
        self.assertIn("measures neither profile", text)

    def test_a_node_with_no_cell_says_nothing(self):
        rows = self._rows([(0, 4.1), (4, 3.9)])
        for r in rows:
            r["battery_v"] = ""
        self.assertEqual(soak.discharge(rows), [])

    def test_a_csv_without_the_columns_says_nothing(self):
        rows = self._rows([(0, 4.1), (4, 3.9)])
        for r in rows:
            del r["battery_v"]
        self.assertEqual(soak.discharge(rows), [])

    def test_a_cell_that_did_not_move_is_not_a_rate(self):
        text = "\n".join(soak.discharge(self._rows([(0, 4.000), (1, 4.000)])))
        self.assertIn("did not fall", text)
        self.assertNotIn("mV/h", text)

    def test_the_band_is_timed_between_its_two_crossings(self):
        # Hourly samples: the band spans four of them, which the resolution
        # guard accepts. The sparse version of this run is refused instead, and
        # rightly — see test_a_band_crossed_faster_than_the_sampling_is_refused.
        rows = self._rows([(0, 4.050), (1, 4.000), (2, 3.950), (3, 3.900),
                           (4, 3.850), (5, 3.800), (6, 3.780)])
        text = "\n".join(soak.discharge(rows, band=(4.000, 3.800)))
        self.assertIn("band 4.000-3.800 V crossed in 4.00 h", text)

    def test_a_band_the_run_never_reached_says_so(self):
        rows = self._rows([(0, 4.050), (4, 3.950)])
        text = "\n".join(soak.discharge(rows, band=(4.000, 3.500)))
        self.assertIn("not fully covered", text)
        self.assertNotIn("crossed in", text)


class BatterySampling(unittest.TestCase):
    def test_the_power_object_is_recorded(self):
        body = _status()
        body["power"] = {"profile": "battery", "cpu_mhz": 80, "pmu": "AXP2101",
                         "battery_present": True, "battery_v": 3.978,
                         "battery_pct": 61, "battery_charging": False}
        with _mocked_node(body):
            row = soak.sample("node")
        self.assertEqual(row["power_profile"], "battery")
        self.assertEqual(row["pmu"], "AXP2101")
        self.assertEqual(row["battery_v"], 3.978)
        self.assertEqual(row["battery_charging"], 0)
        self.assertEqual(row["battery_present"], 1)

    def test_a_board_that_cannot_tell_records_blank_not_false(self):
        # chargeKnown false sends null. Recording that as 0 would let a run on
        # USB be read as a discharge measurement.
        body = _status()
        body["power"] = {"profile": "performance", "battery_charging": None,
                         "battery_v": 4.1}
        with _mocked_node(body):
            row = soak.sample("node")
        self.assertEqual(row["battery_charging"], "")
        self.assertNotEqual(row["battery_charging"], 0)

    def test_a_board_with_no_power_object_records_blanks(self):
        with _mocked_node(_status()):
            row = soak.sample("node")
        for k in ("power_profile", "pmu", "battery_v", "battery_charging"):
            self.assertEqual(row[k], "", f"{k} was {row[k]!r}")

class TheBandSurvivesARealCell(unittest.TestCase):
    """The band time is the figure that will pick a shipped default, and every
    case here made it print `0.00 h` — or a truncated traversal — while still
    labelling itself comparable."""

    def _rows(self, samples, profile="battery"):
        out = []
        for h, v in samples:
            out.append(dict(
                ts=f"2026-09-08T{int(h):02d}:{int(round((h % 1) * 60)):02d}:00+00:00",
                node="n", reachable=1, uptime_s=int(h * 3600), boot_count=5,
                boot_reason="power-on", heap_free=50000, heap_min=40000,
                heap_largest=30000, stack_lowest=2000, stack_lowest_task="rns",
                power_profile=profile, battery_v=v, battery_present=1,
                battery_charging=0))
        return out

    def test_a_run_that_began_inside_the_band_is_refused(self):
        # It never crossed HIGH, so the traversal is truncated and the time is
        # of a partial band. Reporting it as comparable is the worst case.
        rows = self._rows([(h, 3.900 - h * 0.025) for h in range(5)])
        text = "\n".join(soak.discharge(rows, band=(4.00, 3.80)))
        self.assertIn("already at or below", text)
        self.assertNotIn("crossed in", text)

    def test_a_transient_load_sag_is_not_a_crossing(self):
        # The case that will actually happen: a TX burst sags the cell 200 mV
        # and it recovers. One such sample used to collapse the band to 0.00 h,
        # which reads as infinite draw.
        samples = [(0, 4.100), (1, 3.790), (2, 4.020), (3, 4.000), (4, 3.960),
                   (5, 3.930), (6, 3.900), (7, 3.870), (8, 3.840), (9, 3.810),
                   (10, 3.795), (11, 3.780)]
        text = "\n".join(soak.discharge(self._rows(samples), band=(4.00, 3.80)))
        self.assertIn("crossed in", text)
        hours = float(text.split("crossed in ")[1].split(" h")[0])
        self.assertGreater(hours, 3.0, f"a sag was taken for a crossing: {text}")

    def test_a_band_crossed_faster_than_the_sampling_is_refused(self):
        # Two samples 12 h apart cannot resolve a band that fell between them.
        rows = self._rows([(0, 4.100), (12, 3.700)])
        text = "\n".join(soak.discharge(rows, band=(4.00, 3.80)))
        self.assertIn("too fast to resolve", text)
        self.assertNotIn("compare against another run", text)

    def test_a_crossing_time_is_interpolated_not_snapped(self):
        # 4.00 V falls between the samples at h=0 (4.020) and h=1 (3.980), so
        # the crossing is halfway, not at either sample.
        samples = [(0, 4.020), (1, 3.980), (2, 3.940), (3, 3.900), (4, 3.860),
                   (5, 3.820), (6, 3.790), (7, 3.780)]
        text = "\n".join(soak.discharge(self._rows(samples), band=(4.00, 3.80)))
        hours = float(text.split("crossed in ")[1].split(" h")[0])
        self.assertAlmostEqual(hours, 5.0, delta=0.25, msg=text)

    def test_traffic_is_reported_beside_the_band(self):
        rows = self._rows([(h, 4.050 - h * 0.030) for h in range(10)])
        for i, r in enumerate(rows):
            r["rx_packets"] = 100 + i * 10
            r["tx_packets"] = 5 + i
        text = "\n".join(soak.discharge(rows, band=(4.00, 3.85)))
        self.assertIn("the node did:", text)
        self.assertIn("rx +", text)


class RunsThatAreNotOneRun(unittest.TestCase):
    def _rows(self, samples, boot=5, profile="battery"):
        out = []
        for h, v in samples:
            out.append(dict(
                ts=f"2026-09-08T{int(h):02d}:00:00+00:00", node="n", reachable=1,
                uptime_s=int(h * 3600), boot_count=boot, boot_reason="power-on",
                heap_free=50000, heap_min=40000, heap_largest=30000,
                stack_lowest=2000, stack_lowest_task="rns", power_profile=profile,
                battery_v=v, battery_present=1, battery_charging=0))
        return out

    def test_a_recharge_splits_the_file_into_stretches(self):
        rows = self._rows([(0, 4.100), (1, 4.050), (2, 4.000)])          # run A
        rows += self._rows([(3, 4.180), (4, 4.120), (5, 4.060), (6, 4.000)])  # recharged
        text = "\n".join(soak.discharge(rows))
        self.assertIn("discharge stretches", text)
        self.assertIn("4.180 V", text)                # the longer stretch, not run A

    def test_a_restart_splits_the_run(self):
        rows = self._rows([(0, 4.100), (1, 4.080)], boot=5)
        rows += self._rows([(2, 4.060), (3, 4.040), (4, 4.020)], boot=6)
        text = "\n".join(soak.discharge(rows))
        self.assertIn("discharge stretches", text)

    def test_a_stale_divider_is_not_a_measurement(self):
        # present=0 with a plausible leftover voltage: an ADC board whose
        # converter stopped still publishes its last reading.
        rows = self._rows([(0, 3.100), (6, 3.050)])
        for r in rows:
            r["battery_present"] = 0
        text = "\n".join(soak.discharge(rows))
        self.assertIn("no cell", text)
        self.assertNotIn("mV/h", text)

    def test_a_long_gap_in_sampling_is_flagged(self):
        rows = self._rows([(0, 4.100), (1, 4.090), (9, 3.900), (10, 3.890)])
        text = "\n".join(soak.discharge(rows))
        self.assertIn("not observed", text)

    def test_a_board_that_cannot_tell_about_charging_is_warned_about(self):
        rows = self._rows([(0, 4.100), (4, 3.900)])
        for r in rows:
            r["battery_charging"] = ""
        text = "\n".join(soak.discharge(rows))
        self.assertIn("cannot tell whether it was charging", text)

    def test_a_python_true_counts_as_charging(self):
        # str(True) == "True": the old ad-hoc decode missed it and reported a
        # rate for a node on USB.
        rows = self._rows([(0, 3.900), (4, 4.100)])
        for r in rows:
            r["battery_charging"] = True
        text = "\n".join(soak.discharge(rows))
        self.assertIn("was charging", text)
        self.assertNotIn("mV/h", text)

    def test_a_mixed_profile_run_reports_no_figure_at_all(self):
        rows = self._rows([(0, 4.100), (2, 4.000)], profile="battery")
        rows += self._rows([(3, 3.950), (4, 3.900)], profile="performance")
        text = "\n".join(soak.discharge(rows, band=(4.05, 3.90)))
        self.assertIn("measures neither profile", text)
        self.assertNotIn("mV/h", text)
        self.assertNotIn("crossed in", text)



if __name__ == "__main__":
    unittest.main()
