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
        # The firmware omits these keys entirely when the RTC domain did not
        # hold. Recording 0 here would report a power cut as a clean run.
        with _mocked_node(_status(prev_uptime_s=None)):
            row = soak.sample("node")
        del row["ts"]
        self.assertEqual(row["reachable"], 1)
        self.assertEqual(row["prev_alloc_failures"], "")
        self.assertEqual(row["prev_contained"], "")
        self.assertNotEqual(row["prev_alloc_failures"], 0)

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

    def test_the_new_columns_are_last_so_old_files_still_parse(self):
        # The file's own rule: appending anywhere but the end moves every
        # column after it out from under a CSV written before the change.
        self.assertEqual(soak.FIELDS[-2:], ["prev_alloc_failures", "prev_contained"])


if __name__ == "__main__":
    unittest.main()
