# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
#
# The telemetry exporter's two hard problems, neither of which is arithmetic.
#
# **The data is sparse in time and Prometheus is not.** A scrape happens every
# fifteen seconds; a LoRa node answers every five minutes if it answers at all.
# An exporter that simply held the last value would draw a confident flat line
# for a node that fell off a hill three days ago — the most misleading thing it
# could possibly do, because a flat line reads as a healthy sensor. So the
# tests below care less about whether a battery percentage comes out right than
# about whether it *stops* coming out.
#
# **Absent is not zero.** The firmware is careful: a board that cannot see its
# charger sends nil rather than false, and a node whose previous run left no
# record says "unknown" rather than 0. Both distinctions die to one `or 0`
# somewhere in here, silently, and the graph afterwards looks fine — which is
# how a Wireless Bridge panicked seven times in a week while every reading said
# "0 allocation failures".

import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.abspath(os.path.join(_HERE, os.pardir))
_METRICS = os.path.join(_TOOLS, "metrics")
for _path in (_TOOLS, _METRICS):
    if _path not in sys.path:
        sys.path.insert(0, _path)


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


wire = _load("lxmf_wire", os.path.join(_TOOLS, "lxmf_wire.py"))
exposition = _load("exposition", os.path.join(_METRICS, "exposition.py"))
state = _load("state", os.path.join(_METRICS, "state.py"))
state_module = state

NODE = "0e202b2f65e925da0a82abda88181eb9"
OTHER = "5e633f2f81f367493a3339896e7cedbd"

# A node with everything, as lxmf_wire hands it over. Kept small on purpose:
# the wire shapes are pinned in test_lxmf_wire.py against the firmware's own
# bytes, and repeating them here would be a second definition to keep in step.
READINGS = {
    "clock_seconds": 1767225600.0,
    "information": "RetiMesh Node v0.1.0 (LilyGO T3-S3)",
    "firmware_version": "v0.1.0",
    "board": "LilyGO T3-S3",
    "battery_percent": 87.5,
    "battery_charging": True,
    "latitude_degrees": 42.6977,
    "longitude_degrees": 23.3219,
    "ram_capacity_bytes": 327680.0,
    "ram_used_bytes": 228164.0,
    "storage": {"flash": {"capacity_bytes": 3145728.0, "used_bytes": 1751662.0},
                "card": {"capacity_bytes": 15626174464.0, "used_bytes": 450789376.0}},
}


def samples(text, name):
    """Every sample of one metric, as {labels-without-node: value}."""
    out = {}
    for line in text.splitlines():
        if line.startswith("#") or not line.startswith(name):
            continue
        head, _, value = line.rpartition(" ")
        if not head.startswith(name + "{") and head != name:
            continue                      # a longer metric name that shares a prefix
        labels = head[len(name):].strip("{}")
        out[labels] = float(value)
    return out


def series(text, name):
    """Whether a metric has any sample at all — the question most of these ask."""
    return bool(samples(text, name))


class TheExpositionFormat(unittest.TestCase):
    """A malformed document makes Prometheus drop the whole scrape, so the
    failure is a fleet that vanishes rather than one metric that misbehaves."""

    def test_every_family_is_declared_once_before_its_samples(self):
        e = exposition.Exposition()
        e.gauge("a_gauge", "help", 1, {"node": "x"})
        e.gauge("a_gauge", "help", 2, {"node": "y"})
        e.counter("a_counter_total", "help", 3)
        text = e.render()
        self.assertEqual(text.count("# TYPE a_gauge gauge"), 1)
        self.assertEqual(text.count("# HELP a_gauge help"), 1)
        body = [ln for ln in text.splitlines() if not ln.startswith("#")]
        self.assertEqual(len(body), 3)

    def test_a_label_value_with_quotes_or_backslashes_is_escaped(self):
        e = exposition.Exposition()
        e.info("a_info", "help", {"board": 'a "quoted" \\ board'})
        self.assertIn(r'board="a \"quoted\" \\ board"', e.render())

    def test_the_three_special_numbers_are_spelled_prometheus_s_way(self):
        # Python writes these in lower case and Prometheus will not read them
        # back, which produces an exporter that works until the first board
        # reports a NaN RSSI.
        e = exposition.Exposition()
        e.gauge("a_gauge", "h", float("nan"), {"n": "1"})
        e.gauge("a_gauge", "h", float("inf"), {"n": "2"})
        e.gauge("a_gauge", "h", float("-inf"), {"n": "3"})
        text = e.render()
        self.assertIn("NaN", text)
        self.assertIn("+Inf", text)
        self.assertIn("-Inf", text)

    def test_a_bad_name_is_refused_where_it_is_written_not_at_scrape_time(self):
        e = exposition.Exposition()
        with self.assertRaises(ValueError):
            e.gauge("not a metric name", "h", 1)
        with self.assertRaises(ValueError):
            e.counter("missing_the_suffix", "h", 1)
        with self.assertRaises(ValueError):
            e.gauge("a_gauge", "h", 1, {"not a label": "x"})

    def test_the_same_series_written_twice_keeps_the_last(self):
        # Two samples of one series in a single document is not a duplicate
        # reading — it is a scrape Prometheus rejects outright, so the whole
        # fleet vanishes rather than one metric misbehaving. promtool calls it
        # "metric not unique", and it is how this rule was found.
        e = exposition.Exposition()
        e.gauge("a_gauge", "h", 1, {"node": "x"})
        e.gauge("a_gauge", "h", 2, {"node": "x"})
        self.assertEqual(samples(e.render(), "a_gauge"), {'node="x"': 2.0})

    def test_a_label_value_that_is_not_text_still_orders_and_renders(self):
        # Ordering a document must never compare a number against a string and
        # raise inside a scrape, which is a fleet that vanishes.
        e = exposition.Exposition()
        e.gauge("a_gauge", "h", 1, {"part": 0})
        e.gauge("a_gauge", "h", 2, {"part": "flash"})
        self.assertEqual(samples(e.render(), "a_gauge"),
                         {'part="0"': 1.0, 'part="flash"': 2.0})

    def test_one_name_cannot_be_two_types(self):
        e = exposition.Exposition()
        e.gauge("a_thing", "h", 1)
        with self.assertRaises(ValueError):
            e.counter("a_thing", "h", 1)


class ANodeThatGoesQuiet(unittest.TestCase):
    def setUp(self):
        self.fleet = state.Fleet(max_age=900, console_max_age=2700)
        node = self.fleet.node(NODE, name="hilltop", polled=True, now=0.0)
        node.asked("telemetry")
        node.ingest_telemetry(READINGS, 1000.0)

    def test_a_fresh_reading_is_published(self):
        text = self.fleet.render(now=1100.0).render()
        self.assertEqual(samples(text, "retimesh_node_battery_percent"),
                         {'node="%s"' % NODE: 87.5})
        self.assertEqual(samples(text, "retimesh_node_up"), {'node="%s"' % NODE: 1.0})

    def test_a_stale_reading_stops_being_published_entirely(self):
        # Not held flat, and not zeroed: gone, so Prometheus marks the series
        # stale and a graph of it ends rather than lying.
        text = self.fleet.render(now=1000.0 + 901).render()
        self.assertFalse(series(text, "retimesh_node_battery_percent"))
        self.assertFalse(series(text, "retimesh_node_position_latitude_degrees"))

    def test_but_the_node_does_not_disappear_with_its_readings(self):
        # "Known and silent" has to be visible as something other than an
        # absence — this is the state somebody is trying to diagnose.
        text = self.fleet.render(now=1000.0 + 901).render()
        self.assertEqual(samples(text, "retimesh_node_up"), {'node="%s"' % NODE: 0.0})
        self.assertTrue(series(text, "retimesh_node_last_reply_timestamp_seconds"))
        self.assertTrue(series(text, "retimesh_node_requests_total"))
        self.assertTrue(series(text, "retimesh_node_info"))

    def test_the_name_and_board_survive_the_silence(self):
        # Without them a silent node has no board and no name on any panel that
        # joins against this — exactly when somebody is working out which board
        # on which hill has stopped.
        text = self.fleet.render(now=1000.0 + 901).render()
        line = [ln for ln in text.splitlines() if ln.startswith("retimesh_node_info")][0]
        self.assertIn('name="hilltop"', line)

    def test_zero_never_expires_which_is_only_right_on_a_bench(self):
        forever = state.Fleet(max_age=0)
        node = forever.node(NODE, now=0.0)
        node.ingest_telemetry(READINGS, 1000.0)
        text = forever.render(now=1000.0 + 86400).render()
        self.assertTrue(series(text, "retimesh_node_battery_percent"))

    def test_the_console_keeps_its_own_window(self):
        # The console is asked far less often, because it costs the node a
        # longer reply. One window sized for the slower channel would leave a
        # dead node's battery on a dashboard for three quarters of an hour.
        node = self.fleet.nodes[NODE]
        node.ingest_console(["RM POWER volts=4.012"], 1000.0)
        text = self.fleet.render(now=1000.0 + 1500).render()
        self.assertFalse(series(text, "retimesh_node_battery_percent"))
        self.assertTrue(series(text, "retimesh_node_battery_volts"))


class ReadingsAreReplacedWholesale(unittest.TestCase):
    def test_a_fix_the_node_has_lost_stops_being_reported(self):
        # Merging would leave the last known position on the map forever, which
        # is the same lie as a flat battery line.
        fleet = state.Fleet(max_age=900)
        node = fleet.node(NODE, now=0.0)
        node.ingest_telemetry(READINGS, 1000.0)
        without = dict(READINGS)
        del without["latitude_degrees"]
        del without["longitude_degrees"]
        node.ingest_telemetry(without, 1100.0)
        text = fleet.render(now=1100.0).render()
        self.assertFalse(series(text, "retimesh_node_position_latitude_degrees"))
        self.assertTrue(series(text, "retimesh_node_battery_percent"))


class AbsentIsNotZero(unittest.TestCase):
    def setUp(self):
        self.fleet = state.Fleet(max_age=900)
        self.node = self.fleet.node(NODE, now=0.0)

    def render(self):
        return self.fleet.render(now=1000.0).render()

    def test_a_charger_the_board_cannot_see_publishes_no_series(self):
        # A false here reads as "plugged in and not taking charge", which sends
        # somebody looking for a fault in a working cable.
        readings = dict(READINGS)
        del readings["battery_charging"]
        self.node.ingest_telemetry(readings, 1000.0)
        text = self.render()
        self.assertTrue(series(text, "retimesh_node_battery_percent"))
        self.assertFalse(series(text, "retimesh_node_battery_charging"))

    def test_a_charger_that_is_seen_and_idle_publishes_a_zero(self):
        readings = dict(READINGS, battery_charging=False)
        self.node.ingest_telemetry(readings, 1000.0)
        self.assertEqual(samples(self.render(), "retimesh_node_battery_charging"),
                         {'node="%s"' % NODE: 0.0})

    def test_an_unknown_previous_run_publishes_nothing_rather_than_a_zero(self):
        # The current run's counters were zeroed by the restart being
        # investigated, so the previous run is the only place a crash can still
        # be explained — and "unknown" means nothing survived to report, which
        # is a power cut rather than a clean run.
        self.node.ingest_console(
            ["RM STATUS prev_uptime_s=unknown prev_alloc_failures=unknown "
             "prev_contained=unknown"], 1000.0)
        text = self.render()
        self.assertFalse(series(text, "retimesh_node_previous_run_alloc_failures"))
        self.assertFalse(series(text, "retimesh_node_previous_run_seconds"))

    def test_a_known_clean_previous_run_publishes_its_zeros(self):
        self.node.ingest_console(
            ["RM STATUS prev_uptime_s=86400 prev_alloc_failures=0 prev_contained=0"],
            1000.0)
        text = self.render()
        self.assertEqual(
            samples(text, "retimesh_node_previous_run_alloc_failures"),
            {'node="%s"' % NODE: 0.0})
        self.assertEqual(samples(text, "retimesh_node_previous_run_seconds"),
                         {'node="%s"' % NODE: 86400.0})


class TheConsoleChannel(unittest.TestCase):
    def setUp(self):
        self.fleet = state.Fleet(max_age=900)
        self.node = self.fleet.node(NODE, now=0.0)

    def render(self):
        return self.fleet.render(now=1000.0).render()

    def test_stacks_names_its_tasks_in_the_keys(self):
        self.node.ingest_console(
            ["RM STACKS tightest=radio headroom=2048",
             "RM STACKS loopTask=3000 radio=2048 rns=6000",
             "RM OK STACKS lines=2"], 1000.0)
        text = self.render()
        got = samples(text, "retimesh_node_task_stack_headroom_bytes")
        self.assertEqual(sorted(got.values()), [2048.0, 3000.0, 6000.0])
        self.assertEqual(samples(text, "retimesh_node_stack_tightest_headroom_bytes"),
                         {'node="%s",task="radio"' % NODE: 2048.0})

    def test_a_reply_spanning_several_lines_is_gathered_before_it_replaces(self):
        # STACKS packs its tasks onto as few lines as the node's buffer allows,
        # and the closing OK carries no readings. Letting either replace what
        # the other said would leave one line's worth of tasks.
        self.node.ingest_console(["RM STACKS loopTask=3000"], 1000.0)
        self.node.ingest_console(
            ["RM STACKS radio=2048", "RM STACKS rns=6000", "RM OK STACKS"], 1000.0)
        got = samples(self.render(), "retimesh_node_task_stack_headroom_bytes")
        self.assertEqual(sorted(got.values()), [2048.0, 6000.0])

    def test_a_refusal_is_counted_so_a_missing_enrolment_is_visible(self):
        # 403 means this exporter is not enrolled as an administrator on the
        # node. Uncounted it is indistinguishable from a node that never
        # answers, and the operator goes looking at the radio.
        self.node.ingest_console(["RM ERR ADMIN 403 not_an_administrator"], 1000.0)
        self.assertEqual(
            samples(self.render(), "retimesh_node_console_errors_total"),
            {'code="403",command="ADMIN",node="%s"' % NODE: 1.0})

    def test_strings_become_labels_on_an_info_metric_not_values(self):
        self.node.ingest_console(
            ['RM POWER profile=balanced cpu_mhz=240 uptime_s=86400',
             'RM POWER volts=4.012 percent=87 charging=yes stale=no pmu="AXP2101"'],
            1000.0)
        text = self.render()
        line = [ln for ln in text.splitlines() if ln.startswith("retimesh_node_power_info")][0]
        self.assertIn('profile="balanced"', line)
        self.assertIn('pmu="AXP2101"', line)
        self.assertEqual(samples(text, "retimesh_node_battery_volts"),
                         {'node="%s"' % NODE: 4.012})
        self.assertEqual(samples(text, "retimesh_node_uptime_seconds"),
                         {'node="%s"' % NODE: 86400.0})

    def test_a_key_nobody_has_decided_the_meaning_of_is_ignored(self):
        # Exposing every key that happens to parse as a number would produce
        # metrics without units, and a key's meaning is not in its name:
        # heap_free is internal memory and dram_free is the byte-addressable
        # part of the same, and only one says whether a task can be placed.
        self.node.ingest_console(["RM POWER percent=87 charging=yes"], 1000.0)
        text = self.render()
        self.assertNotIn("percent=", text)
        self.assertFalse(series(text, "retimesh_node_battery_percent"))

    def test_two_commands_carrying_one_fact_publish_one_series(self):
        # POWER and STATUS both report uptime. Published twice it is a scrape
        # Prometheus refuses; published once it has to be the fresher of the
        # two, not whichever command sorts later in the alphabet.
        self.node.ingest_console(["RM STATUS uptime_s=1000"], 500.0)
        self.node.ingest_console(["RM POWER uptime_s=1600 volts=4.0"], 1000.0)
        self.assertEqual(samples(self.render(), "retimesh_node_uptime_seconds"),
                         {'node="%s"' % NODE: 1600.0})

    def test_the_older_command_does_not_overwrite_the_newer_one(self):
        self.node.ingest_console(["RM POWER uptime_s=1600 volts=4.0"], 500.0)
        self.node.ingest_console(["RM STATUS uptime_s=2000"], 1000.0)
        self.assertEqual(samples(self.render(), "retimesh_node_uptime_seconds"),
                         {'node="%s"' % NODE: 2000.0})

    def test_one_info_metric_per_node_however_many_commands_report_it(self):
        # POWER and STATUS both report `profile`. Two series of
        # retimesh_node_power_info on one node is not a duplicate series, so
        # nothing rejects the scrape — and then every `group_left(profile)` in
        # the dashboard fails with "found duplicate series for the match group",
        # which is a panel that breaks rather than a metric that misbehaves.
        self.node.ingest_console(["RM STATUS power profile=balanced wifi_ps=min"], 500.0)
        self.node.ingest_console(['RM POWER profile=eco pmu="AXP2101"'], 1000.0)
        text = self.render()
        lines = [ln for ln in text.splitlines()
                 if ln.startswith("retimesh_node_power_info")]
        self.assertEqual(len(lines), 1, lines)
        # Merged across both, freshest winning where they disagree.
        self.assertIn('profile="eco"', lines[0])
        self.assertIn('pmu="AXP2101"', lines[0])
        self.assertIn('wifi_ps="min"', lines[0])

    def test_the_three_things_a_board_can_see_of_its_cell(self):
        # "stale" (the converter stopped answering) and "not-seen" (none
        # fitted) both report no battery and send an operator to opposite ends
        # of the board, so they must not collapse into one another.
        for state, expected in (("present", 0.0), ("stale", 1.0)):
            fleet = state_module.Fleet(max_age=900)
            node = fleet.node(NODE, now=0.0)
            node.ingest_console(["RM STATUS battery=%s volts=4.012 percent=87" % state],
                                1000.0)
            text = fleet.render(now=1000.0).render()
            self.assertEqual(samples(text, "retimesh_node_battery_reading_stale"),
                             {'node="%s"' % NODE: expected})
            self.assertIn('state="%s"' % state, text)

    def test_a_board_with_no_cell_has_no_reading_to_be_stale(self):
        self.node.ingest_console(["RM STATUS battery=not-seen volts=0.000 percent=0"],
                                 1000.0)
        text = self.render()
        self.assertFalse(series(text, "retimesh_node_battery_reading_stale"))
        self.assertIn('state="not-seen"', text)      # still said, just not as 0

    def test_a_board_with_no_cell_publishes_no_voltage_rather_than_zero(self):
        # The divider still reads, and it reads 0.000 V. Published, that puts a
        # node on the floor of every voltage graph and drags any fleet-wide
        # minimum down with it — a fabricated reading, which is worse than a
        # missing one because a missing one is visibly missing.
        self.node.ingest_console(["RM STATUS battery=not-seen volts=0.000 percent=0"],
                                 1000.0)
        text = self.render()
        self.assertFalse(series(text, "retimesh_node_battery_volts"))
        self.assertIn('state="not-seen"', text)

    def test_this_run_s_faults_and_the_previous_run_s_are_different_metrics(self):
        # One says what is going wrong now; the other is the only surviving
        # explanation of the restart being investigated, because the restart
        # zeroed the first.
        self.node.ingest_console(
            ["RM STATUS alloc_failures=3 contained=5 last_ms_ago=900",
             "RM STATUS prev_uptime_s=600 prev_alloc_failures=11 prev_contained=12"],
            1000.0)
        text = self.render()
        self.assertEqual(samples(text, "retimesh_node_alloc_failures"),
                         {'node="%s"' % NODE: 3.0})
        self.assertEqual(samples(text, "retimesh_node_previous_run_alloc_failures"),
                         {'node="%s"' % NODE: 11.0})

    def test_volts_from_status_where_the_firmware_has_no_power_command(self):
        # STATUS carries the cell on firmware without POWER, and the exporter
        # must not need the newer command to report a discharge.
        self.node.ingest_console(["RM STATUS battery=present volts=3.874 percent=61"],
                                 1000.0)
        self.assertEqual(samples(self.render(), "retimesh_node_battery_volts"),
                         {'node="%s"' % NODE: 3.874})

    def test_online_and_offline_become_one_and_zero(self):
        self.node.ingest_console(["RM STATUS radio=offline model=SX1262"], 1000.0)
        text = self.render()
        self.assertEqual(samples(text, "retimesh_node_radio_online"),
                         {'node="%s"' % NODE: 0.0})
        self.assertIn('model="SX1262"', text)


class NodesTheExporterWasNotToldAbout(unittest.TestCase):
    def test_a_node_that_only_volunteers_is_kept_and_marked(self):
        # Sideband can be told to push its own telemetry, and a node enrolled
        # against this address may too. "Silent node we ask" and "node that
        # stopped volunteering" want different alerts.
        fleet = state.Fleet(max_age=900)
        fleet.node(NODE, polled=True, now=0.0)
        fleet.node(OTHER, now=0.0).ingest_telemetry(READINGS, 1000.0)
        got = samples(fleet.render(now=1000.0).render(), "retimesh_node_polled")
        self.assertEqual(got, {'node="%s"' % NODE: 1.0, 'node="%s"' % OTHER: 0.0})

    def test_an_unnamed_node_still_gets_a_usable_handle(self):
        # A dashboard legend keyed on `name` should read as something rather
        # than as a gap. Naming the node later changes only this metric's
        # labels, never the identity of any reading.
        fleet = state.Fleet(max_age=900)
        fleet.node(OTHER, now=0.0)
        line = [ln for ln in fleet.render(now=1000.0).render().splitlines()
                if ln.startswith("retimesh_node_info")][0]
        self.assertIn('name="%s"' % OTHER[:8], line)

    def test_a_request_that_never_left_is_still_recorded(self):
        # Silence is a result: a node that was asked and did not answer is a
        # different fact from one that was never asked.
        fleet = state.Fleet(max_age=900)
        node = fleet.node(NODE, polled=True, now=0.0)
        node.asked("telemetry")
        node.failed("no_path")
        text = fleet.render(now=1000.0).render()
        self.assertEqual(samples(text, "retimesh_node_requests_total"),
                         {'channel="telemetry",node="%s"' % NODE: 1.0})
        self.assertEqual(samples(text, "retimesh_node_request_failures_total"),
                         {'node="%s",reason="no_path"' % NODE: 1.0})
        self.assertEqual(samples(text, "retimesh_node_up"), {'node="%s"' % NODE: 0.0})


class TheCallbacksLxmfFleetWillActuallyCall(unittest.TestCase):
    """Duck typing across a module boundary, so nothing checks these but this.

    lxmf_fleet.Client calls `on_message(source, telemetry, text)` and
    `on_failure(node_hex, channel, reason, tag)`. A handler with the wrong
    arity raises inside RNS's own thread on the first node that has no path —
    which is every node, on the first round, before anything has been
    collected. The exporter crash-looped exactly that way once.

    Both modules import without RNS installed, because each defers its
    `import lxmf_fleet` into the constructor. That is what makes this checkable
    here at all.
    """

    def _handlers(self):
        exporter = _load("exporter", os.path.join(_METRICS, "exporter.py"))
        collector = _load("collector", os.path.join(
            _TOOLS, "soak", "collector.py"))
        return ((exporter.Exporter, collector.Collector))

    def test_both_readers_accept_what_the_client_hands_them(self):
        import inspect
        for cls in self._handlers():
            message = inspect.signature(cls.on_message if hasattr(cls, "on_message")
                                        else cls.on_delivery)
            self.assertEqual(len(message.parameters), 4,        # self + three
                             "%s takes the wrong number of message arguments" % cls)
            failure = inspect.signature(cls.on_failure)
            bound = [p for p in failure.parameters.values()
                     if p.kind is not p.VAR_KEYWORD]
            self.assertEqual(len(bound), 5,                     # self + four
                             "%s takes the wrong number of failure arguments" % cls)
            # The call is positional, so it must not need keywords to work.
            failure.bind(None, "aa", "telemetry", "no_path", "tag")


class TheWholeDocument(unittest.TestCase):
    def test_a_full_fleet_renders_something_prometheus_would_accept(self):
        fleet = state.Fleet(max_age=900)
        node = fleet.node(NODE, name="hilltop", polled=True, now=0.0)
        node.asked("telemetry")
        node.ingest_telemetry(READINGS, 1000.0)
        node.ingest_console(
            ["RM STACKS tightest=radio headroom=2048",
             "RM POWER volts=4.012 profile=balanced uptime_s=86400 pmu=\"AXP2101\"",
             "RM STATUS uptime_s=86400 boot_count=7 radio=online model=SX1262",
             "RM STATUS battery=present volts=4.010 alloc_failures=0 contained=0",
             "RM STATUS power profile=balanced wifi_ps=min"],
            1000.0)
        fleet.node(OTHER, polled=True, now=0.0).failed("delivery_failed")
        text = fleet.render(now=1000.0).render()

        declared, typed = set(), {}
        for line in text.splitlines():
            if line.startswith("# HELP "):
                name = line.split(" ", 2)[2].split(" ", 1)[0]
                self.assertNotIn(name, declared, "%s declared twice" % name)
                declared.add(name)
            elif line.startswith("# TYPE "):
                _, _, rest = line.split(" ", 2)
                name, kind = rest.split(" ", 1)
                typed[name] = kind
            elif line:
                name = line.split("{")[0].split(" ")[0]
                self.assertIn(name, typed, "%s has no TYPE before its samples" % name)
        for name, kind in typed.items():
            if kind == "counter":
                self.assertTrue(name.endswith("_total"), name)
        self.assertTrue(text.endswith("\n"))

        # No series twice. This is the whole-document form of the rule above,
        # and the one promtool enforces as "metric not unique".
        seen = set()
        for line in text.splitlines():
            if line.startswith("#") or not line:
                continue
            series_id = line.rpartition(" ")[0]
            self.assertNotIn(series_id, seen, "%s appears twice" % series_id)
            seen.add(series_id)


if __name__ == "__main__":
    unittest.main()
