#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""What the exporter knows about a fleet, and how that becomes metrics.

Everything here is pure: state goes in through `ingest_*`, a document comes out
of `render`, and no part of it touches a radio or a clock it was not handed.
That is what lets CI check the awkward rules — and every rule in this file is
awkward, because the whole difficulty of exporting mesh telemetry is that the
data is *sparse in time* and Prometheus is not.

Three of them are worth stating before the code:

**A node that has gone quiet stops publishing readings.** A scrape happens
every fifteen seconds; a LoRa node answers every five minutes if it answers at
all. Holding the last battery percentage forever would draw a confident flat
line for a node that fell off a hill three days ago — the single most
misleading thing this exporter could do. After `max_age` the readings are
withheld and Prometheus marks the series stale. What does *not* go away is
`retimesh_node_up`, the last-reply timestamp and the counters: the node is
still known, and "known and silent" has to be visible as something other than
an absence.

**A reading the board cannot take is absent, not zero.** The firmware is
careful about this — a board that cannot see its charger sends nil rather than
false, because "not charging" sends somebody looking for a fault in a working
cable — and that care is worth nothing if it is flattened here. Absent keys in,
absent series out, all the way through.

**Strings are not values.** Firmware version, board, power profile and reset
reason live on `_info` metrics carrying a constant 1, to be brought alongside
the numbers with `group_left`. Hanging them on every metric as labels would
give every series a new identity the moment a node was upgraded, breaking the
graph that was meant to show the upgrade.
"""

from __future__ import annotations

import collections
import time

import lxmf_wire
from exposition import Exposition

# --- the console channel, as metrics ------------------------------------------
#
# Telemetry is open to anyone; the console needs this collector enrolled as an
# administrator on the node. So everything below is a bonus, absent on a fleet
# that has not enrolled it, and nothing in the telemetry path depends on it.
#
# The table is deliberately explicit rather than "expose every key that parses
# as a number". Automatic naming would produce metrics without units, and a
# key's meaning is not in its name: `heap_free` is bytes of *internal* memory
# and `dram_free` is the byte-addressable part of the same, and only one of
# them says whether another task can be placed. A key that is not here is
# ignored, which is the honest outcome for a reading nobody has decided the
# meaning of.


def _number(text):
    """A console value as a float, or None for one that is not a number.

    This is where `prev_alloc_failures=unknown` becomes an absent series rather
    than a zero, and that is not a detail. A node whose RTC domain dropped
    reports no previous-run figures at all; a node whose previous run was clean
    reports zeros. Collapsing the two is the bug this column exists to catch —
    a Wireless Bridge panicked seven times in a week while every reading said
    "0 allocation failures", because every reading was taken after the reboot
    that zeroed the counter, and the 0 was believed.
    """
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


_TRUTHS = {"yes": 1.0, "on": 1.0, "true": 1.0, "online": 1.0,
           "no": 0.0, "off": 0.0, "false": 0.0, "offline": 0.0}


def _boolean(text):
    return _TRUTHS.get(str(text).strip().lower())


# STATUS says "battery=present|stale|not-seen" where POWER says "stale=yes|no".
# "stale" and "not-seen" both mean no usable reading and send an operator to
# opposite ends of the board, so they must not collapse: a board with no cell
# fitted has no reading to *be* stale, and reports nothing here.
_BATTERY_SENSE = {"stale": 1.0, "present": 0.0}


def _stale(text):
    return _BATTERY_SENSE.get(str(text).strip().lower())


# (command, key) -> (metric, type, help, converter)
CONSOLE_GAUGES = {
    ("POWER", "volts"): (
        "retimesh_node_battery_volts", "gauge",
        "Cell voltage as the node measures it. The figure a discharge rate is "
        "taken from, since no board this firmware runs on can measure its own draw",
        _number),
    ("POWER", "stale"): (
        "retimesh_node_battery_reading_stale", "gauge",
        "1 when the node's last battery reading is older than it should be",
        _boolean),
    ("POWER", "uptime_s"): (
        "retimesh_node_uptime_seconds", "gauge",
        "Seconds since the node last restarted", _number),

    # The cell, where STATUS carries it. Firmware that has the POWER command
    # says the same things there; whichever answered most recently wins, and
    # neither is published twice.
    ("STATUS", "volts"): (
        "retimesh_node_battery_volts", "gauge",
        "Cell voltage as the node measures it. The figure a discharge rate is "
        "taken from, since no board this firmware runs on can measure its own draw",
        _number),
    ("STATUS", "battery"): (
        "retimesh_node_battery_reading_stale", "gauge",
        "1 when the node's last battery reading is older than it should be. "
        "Absent on a board with no cell fitted, which has no reading to be stale",
        _stale),
    # The *current* run's faults. The previous run's are below, and they answer
    # different questions: this one is what is going wrong now, that one is the
    # only surviving explanation of the restart being investigated.
    ("STATUS", "alloc_failures"): (
        "retimesh_node_alloc_failures", "gauge",
        "Allocations this run has failed to make. Zeroed by every restart, so "
        "a node that died of memory exhaustion answers 0 a minute later",
        _number),
    ("STATUS", "contained"): (
        "retimesh_node_contained_faults", "gauge",
        "Exceptions this run has caught. Counts every contained fault, not only "
        "allocation failures, so on its own it is not memory pressure", _number),

    ("STATUS", "uptime_s"): (
        "retimesh_node_uptime_seconds", "gauge",
        "Seconds since the node last restarted", _number),
    ("STATUS", "boot_count"): (
        "retimesh_node_boots_total", "counter",
        "Restarts the node has recorded, across the whole life of the board", _number),
    # The previous run, which is the only place a crash can still be explained:
    # the current run's counters were zeroed by the restart being investigated.
    ("STATUS", "prev_uptime_s"): (
        "retimesh_node_previous_run_seconds", "gauge",
        "How long the run before this one lasted. Absent, not zero, when "
        "nothing survived the restart — a power cut rather than a crash", _number),
    ("STATUS", "prev_alloc_failures"): (
        "retimesh_node_previous_run_alloc_failures", "gauge",
        "Allocations the previous run failed to make. Non-zero beside an "
        "unclean boot means it ran out of byte-addressable DRAM", _number),
    ("STATUS", "prev_contained"): (
        "retimesh_node_previous_run_contained_faults", "gauge",
        "Exceptions the previous run caught. Counts every contained fault, not "
        "only allocation failures, so on its own it is not memory pressure", _number),
    ("STATUS", "heap_free"): (
        "retimesh_node_heap_free_bytes", "gauge",
        "Free internal heap. Counts 32-bit-only IRAM as well, so it reads "
        "healthy on a board that can no longer place a task — see the dram_ figures",
        _number),
    ("STATUS", "heap_min"): (
        "retimesh_node_heap_min_free_bytes", "gauge",
        "Least free internal heap seen this run. Telemetry sends the "
        "instantaneous figure, so this is the one that catches the dips", _number),
    ("STATUS", "largest_block"): (
        "retimesh_node_heap_largest_block_bytes", "gauge",
        "Largest single allocation the heap could still satisfy. Fragmentation "
        "shows in the gap between this and the free figure long before a failure",
        _number),
    ("STATUS", "psram_free"): (
        "retimesh_node_psram_free_bytes", "gauge", "Free PSRAM", _number),
    ("STATUS", "dram_free"): (
        "retimesh_node_dram_free_bytes", "gauge",
        "Free byte-addressable DRAM: what a stack or a buffer must come from",
        _number),
    ("STATUS", "dram_min"): (
        "retimesh_node_dram_min_free_bytes", "gauge",
        "Least free byte-addressable DRAM seen this run", _number),
    ("STATUS", "dram_largest_block"): (
        "retimesh_node_dram_largest_block_bytes", "gauge",
        "Largest single byte-addressable DRAM block still available", _number),
    ("STATUS", "rx"): (
        "retimesh_node_lora_rx_packets_total", "counter",
        "LoRa packets received this run", _number),
    ("STATUS", "tx"): (
        "retimesh_node_lora_tx_packets_total", "counter",
        "LoRa packets transmitted this run", _number),
    # Carrier sense is the part of the radio that fails silently: a probe
    # nobody answers reads as a busy channel, so a node whose CAD has stopped
    # keeps routing and keeps counting while deferring before every packet.
    # Any value at all is a fault, whatever the traffic.
    ("STATUS", "cad_timeouts"): (
        "retimesh_node_cad_timeouts_total", "counter",
        "Carrier-sense probes that timed out. Zero on a healthy node whatever "
        "the traffic, so any value at all is a fault", _number),
    ("STATUS", "cad_arm_errors"): (
        "retimesh_node_cad_arm_errors_total", "counter",
        "Failures to arm carrier sense. As above: zero, or a fault", _number),
    ("STATUS", "radio"): (
        "retimesh_node_radio_online", "gauge",
        "1 while the node's own radio reports itself online", _boolean),
}

# Info metrics: (metric, help, {console key: label name}). Emitted once per
# node when at least one of the keys has been seen, from whichever command
# supplied it.
#
# Keyed by the console key alone and *not* by the command, which matters more
# than it looks. POWER and STATUS both report `profile`; two entries would put
# two series of retimesh_node_power_info on one node, with different labels —
# not a duplicate series, so nothing would reject the scrape, and then every
# `group_left(profile)` join in the dashboard would fail with "found duplicate
# series for the match group". One series per info metric per node is the whole
# contract these exist to keep.
CONSOLE_INFO = (
    ("retimesh_node_power_info",
     "The power profile the node is running, the part on its battery line, and "
     "what it is doing with Wi-Fi",
     {"profile": "profile", "pmu": "pmu", "wifi_ps": "wifi_ps"}),
    ("retimesh_node_boot_info",
     "Why the node last restarted, in its own words", {"reset": "reset"}),
    ("retimesh_node_radio_info", "The radio the node found", {"model": "model"}),
    ("retimesh_node_battery_sense_info",
     "What the node can see of its cell: present, stale (the converter stopped "
     "answering) or not-seen (none fitted). The last two both report no "
     "battery and send an operator to opposite ends of the board",
     {"battery": "state"}),
)

# STACKS names its tasks in the keys themselves, so no table can list them.
# These are the two keys on that command that are not a task.
_STACKS_META = ("tightest", "headroom", "lines")

# A board with no cell fitted still prints the divider's reading, and it is
# 0.000 V. That is not a measurement — it is the absence of one, and publishing
# it would put a node on the floor of every voltage graph and drag any
# fleet-wide minimum down with it. The node says which case it is on the same
# line (`battery=not-seen`), so the figures that describe a cell are dropped
# when there is no cell to describe. `battery` itself is kept: it feeds the
# sense info, where "not-seen" is the useful answer.
_NO_CELL = "not-seen"
_CELL_FIGURES = ("volts", "percent")


class Node:
    """One node, and every fact the exporter is holding about it."""

    def __init__(self, address, name="", polled=False, now=None):
        self.address = address
        self.name = name
        self.polled = polled
        self.first_seen = time.time() if now is None else now

        self.readings = {}                 # lxmf_wire.normalise output
        self.readings_at = None
        # What the node *is*, as opposed to what it is reading. Kept apart
        # because it must outlive both silence and a reply that happened not to
        # carry it: a board does not stop being a T3-S3 because it stopped
        # answering, and blanking these would give retimesh_node_info a new
        # series identity at exactly the moment somebody is trying to work out
        # which board on which hill has gone quiet.
        self.identity = {}
        self.console = {}                  # command -> {"at": float, "fields": {}}
        self.last_reply_at = None

        self.requests = collections.Counter()      # channel -> n
        self.replies = collections.Counter()       # channel -> n
        self.failures = collections.Counter()      # reason -> n
        self.console_errors = collections.Counter()  # (command, code) -> n

    # --- what happened ----------------------------------------------------
    def asked(self, channel):
        self.requests[channel] += 1

    def failed(self, reason):
        self.failures[reason] += 1

    def ingest_telemetry(self, readings, now):
        """Replace the readings wholesale.

        Wholesale rather than merged: a sensor the node has stopped reporting —
        a GNSS fix it has lost, a card that has been pulled — must stop being
        reported here too. Merging would leave the last known position on the
        map forever, which is the same lie as a flat battery line.
        """
        self.readings = dict(readings)
        self.readings_at = now
        # Sticky, and only ever overwritten by something a node actually said.
        for key in ("board", "firmware_version", "information"):
            if readings.get(key):
                self.identity[key] = readings[key]
        self.last_reply_at = now
        self.replies["telemetry"] += 1

    def ingest_console(self, lines, now):
        """Console reply lines, grouped by the command they answer.

        One command's data may span several lines — STACKS packs its tasks onto
        as few as the node's buffer allows — so the fields of every line for a
        command are gathered before they replace what was held. Returns the set
        of commands that carried data, so a caller can say what it heard.
        """
        seen = {}
        errors = []
        for line in lines:
            parsed = lxmf_wire.parse_console_line(line)
            if parsed is None:
                continue
            if parsed.kind == "err":
                errors.append((parsed.command, parsed.fields.get("code", "")))
                continue
            seen.setdefault(parsed.command, {}).update(parsed.fields)

        for command, code in errors:
            self.console_errors[(command, code)] += 1
        for command, fields in seen.items():
            if fields:
                self.console[command] = {"at": now, "fields": fields}
        if seen or errors:
            self.last_reply_at = now
            self.replies["console"] += 1
        return set(seen)

    # --- what to say about it ---------------------------------------------
    def fresh(self, at, now, max_age):
        return at is not None and (max_age <= 0 or now - at <= max_age)


class Fleet:
    """Every node the exporter has heard of, and the document it renders."""

    def __init__(self, max_age=0.0, console_max_age=None, max_unsolicited=0):
        # Two windows because the two channels are asked at different rates:
        # telemetry every few minutes, the console far less often because it
        # costs a node's airtime for a longer reply. One window sized for the
        # slower of them would leave a dead node's battery on a dashboard for
        # three quarters of an hour, which is the whole failure this exists to
        # prevent. Zero means never expire, and is only right for a bench.
        self.max_age = max_age
        self.console_max_age = max_age if console_max_age is None else console_max_age
        # How many nodes nobody configured may be held at once. Zero is none,
        # and is the default: on an open mesh anyone can send a readings map,
        # and a store that grows with every stranger is memory this process
        # never gives back and Prometheus cardinality that never falls. Bounded,
        # the worst a mesh full of strangers can do is fill this many slots.
        self.max_unsolicited = max_unsolicited
        self.nodes = {}

    def node(self, address, name=None, polled=None, now=None):
        """The node at that address, created on first sight.

        A node that was never configured but volunteered telemetry is a real
        node and is kept — a phone running Sideband, or a board somebody
        enrolled and forgot. `polled` records which it is, because "silent
        node we ask" and "node that stopped volunteering" want different alerts.
        """
        n = self.nodes.get(address)
        if n is None:
            n = self.nodes[address] = Node(address, name or "", bool(polled),
                                           now=now)
        if name:
            n.name = name
        if polled:
            n.polled = True
        return n

    def unsolicited(self, address, now):
        """Admit a node nobody configured, within the bound. None if refused.

        Least-recently-heard is evicted to make room, so a live sender
        displaces one that stopped talking rather than being turned away
        because a stranger got there first.
        """
        node = self.nodes.get(address)
        if node is not None:
            return node
        if self.max_unsolicited <= 0:
            return None
        strangers = [n for n in self.nodes.values() if not n.polled]
        while len(strangers) >= self.max_unsolicited:
            oldest = min(strangers, key=lambda n: n.last_reply_at or n.first_seen)
            del self.nodes[oldest.address]
            strangers.remove(oldest)
        return self.node(address, now=now)

    def render(self, now=None, into=None):
        now = time.time() if now is None else now
        e = into if into is not None else Exposition()
        for address in sorted(self.nodes):
            self._render_node(e, self.nodes[address], now)
        return e

    def _render_node(self, e, n, now):
        base = {"node": n.address}
        readings = n.readings if n.fresh(n.readings_at, now, self.max_age) else {}

        # Identity first, and always — including for a node that has gone
        # quiet. Taken from `n.identity` rather than from the freshness-filtered
        # readings below: a silent node with no board and no version on it is
        # useless on any panel that joins against this, which is exactly when
        # somebody is trying to work out which board on which hill has stopped.
        # A node nobody named is labelled by the first eight characters of its
        # address rather than by an empty string, so a dashboard legend keyed on
        # `name` reads as something instead of as a gap. Naming it later changes
        # only this metric's labels, never the identity of a reading.
        e.info("retimesh_node_info",
               "The node, by every name it has. Join it onto a reading with "
               "`* on(node) group_left(board, name) retimesh_node_info`",
               dict(base, name=n.name or n.address[:8],
                    board=n.identity.get("board", ""),
                    firmware=n.identity.get("firmware_version", ""),
                    information=n.identity.get("information", "")))

        # Liveness, which outlives the readings on purpose. This is the metric
        # to alert on: it stays at 0 for a node that has stopped answering,
        # where every reading below simply vanishes.
        e.gauge("retimesh_node_up",
                "1 when the node has answered within the exporter's freshness "
                "window, 0 when it is known but silent",
                1 if n.fresh(n.last_reply_at, now, self.max_age) else 0, base)
        e.gauge("retimesh_node_polled",
                "1 for a node this exporter asks, 0 for one that only volunteers",
                1 if n.polled else 0, base)
        if n.last_reply_at is not None:
            e.gauge("retimesh_node_last_reply_timestamp_seconds",
                    "When this exporter last heard anything from the node",
                    n.last_reply_at, base)
        e.gauge("retimesh_node_first_seen_timestamp_seconds",
                "When this exporter first heard of the node", n.first_seen, base)

        for channel, count in n.requests.items():
            e.counter("retimesh_node_requests_total",
                      "Requests sent to the node. Recorded whether or not an "
                      "answer came: silence is a result, and a node that was "
                      "asked and did not answer is a different fact from one "
                      "that was never asked",
                      count, dict(base, channel=channel))
        for channel, count in n.replies.items():
            e.counter("retimesh_node_replies_total",
                      "Replies received from the node, by channel",
                      count, dict(base, channel=channel))
        for reason, count in n.failures.items():
            e.counter("retimesh_node_request_failures_total",
                      "Requests that could not be sent or were not delivered",
                      count, dict(base, reason=reason))
        for (command, code), count in n.console_errors.items():
            # 403 here means this collector is not enrolled as an administrator
            # on that node, which is the setup mistake, and it is otherwise
            # indistinguishable from a node that never answers the console.
            e.counter("retimesh_node_console_errors_total",
                      "Console requests the node refused. A 403 means this "
                      "exporter is not enrolled as an administrator on it",
                      count, dict(base, command=command, code=code))

        self._render_readings(e, base, readings)
        self._render_console(e, n, base, now)

    def _render_readings(self, e, base, r):
        if not r:
            return
        simple = (
            ("clock_seconds", "retimesh_node_clock_timestamp_seconds",
             "The node's own clock. Compare it with `time()`: a node with no "
             "GNSS fix and no RTC stamps 1970, which buries its messages at the "
             "bottom of every client's conversation"),
            ("battery_percent", "retimesh_node_battery_percent",
             "How full the cell is, as the node reads it"),
            ("battery_temperature_celsius", "retimesh_node_battery_temperature_celsius",
             "Cell temperature, where a board has a sensor for it"),
            ("latitude_degrees", "retimesh_node_position_latitude_degrees",
             "Latitude of the node's last fix"),
            ("longitude_degrees", "retimesh_node_position_longitude_degrees",
             "Longitude of the node's last fix"),
            ("altitude_meters", "retimesh_node_position_altitude_meters",
             "Altitude of the node's last fix"),
            ("speed_kmh", "retimesh_node_position_speed_kmh",
             "Speed at the node's last fix"),
            ("bearing_degrees", "retimesh_node_position_bearing_degrees",
             "Bearing at the node's last fix. Always zero from a node that has "
             "no compass, which is every board this firmware runs on"),
            ("accuracy_meters", "retimesh_node_position_accuracy_meters",
             "Claimed accuracy of the node's last fix"),
            ("position_timestamp_seconds", "retimesh_node_position_timestamp_seconds",
             "When the node took the fix it is reporting"),
            ("rssi_dbm", "retimesh_node_rssi_dbm",
             "Signal strength the node heard the request at. A property of the "
             "path from this exporter, not of the node"),
            ("snr_db", "retimesh_node_snr_db",
             "Signal-to-noise ratio the node heard the request at"),
            ("link_quality_percent", "retimesh_node_link_quality_percent",
             "Link quality the node scored the request at"),
            ("cpu_clock_hertz", "retimesh_node_cpu_clock_hertz",
             "Processor clock the node is running at"),
            ("ram_capacity_bytes", "retimesh_node_ram_capacity_bytes",
             "Internal memory the node has. Not the total including PSRAM: on a "
             "board with 8 MB of PSRAM the total reads healthy right up until "
             "the node dies of the kind it actually needs"),
            ("ram_used_bytes", "retimesh_node_ram_used_bytes",
             "Internal memory in use. Instantaneous, not the low-water mark — "
             "the console's STATUS carries that, and the dips it catches are "
             "far below this"),
        )
        for key, name, help_text in simple:
            if key in r:
                e.gauge(name, help_text, r[key], base)

        # Charging is absent, not false, where the board cannot see its charger.
        if "battery_charging" in r:
            e.gauge("retimesh_node_battery_charging",
                    "1 while the cell is taking charge. Absent entirely on a "
                    "board that cannot see its charger, which is not the same "
                    "as 0 and must not be graphed as it",
                    1 if r["battery_charging"] else 0, base)

        for part, figures in sorted(r.get("storage", {}).items()):
            e.gauge("retimesh_node_storage_capacity_bytes",
                    "Storage the node has, per part: internal flash never "
                    "moves, a card fills",
                    figures["capacity_bytes"], dict(base, part=part))
            e.gauge("retimesh_node_storage_used_bytes",
                    "Storage the node has used, per part",
                    figures["used_bytes"], dict(base, part=part))

    def _render_console(self, e, n, base, now):
        # Oldest first, because two commands can carry the same fact — POWER and
        # STATUS both report uptime — and the last one written is the one kept.
        # Rendering in name order instead would let a STATUS from an hour ago
        # overwrite a POWER from a minute ago, for no reason but the alphabet.
        merged = {}
        for command, held in sorted(n.console.items(),
                                    key=lambda kv: (kv[1]["at"], kv[0])):
            if not n.fresh(held["at"], now, self.console_max_age):
                continue
            fields = held["fields"]
            merged.update(fields)
            no_cell = fields.get("battery") == _NO_CELL

            for key, value in sorted(fields.items()):
                if no_cell and key in _CELL_FIGURES:
                    continue           # 0.000 V from a board with no cell is not a reading
                mapped = CONSOLE_GAUGES.get((command, key))
                if not mapped:
                    continue
                name, kind, help_text, convert = mapped
                number = convert(value)
                if number is None:
                    continue           # "unknown" is an absent series, never a 0
                e.sample(name, kind, help_text, number, base)

            if command == "STACKS":
                self._render_stacks(e, base, fields)

        # Once, from the merged view, so a node never carries two series of one
        # info metric — see CONSOLE_INFO for why that would break the joins.
        for name, help_text, keys in CONSOLE_INFO:
            labels = {label: merged[key] for key, label in keys.items()
                      if key in merged}
            if labels:
                e.info(name, help_text, dict(base, **labels))

    def _render_stacks(self, e, base, fields):
        """Per-task stack headroom, whose task names are the keys themselves.

        This is the measurement a soak exists to take. It is a *high-water
        mark* — the least a task has ever had left — so it only falls, and the
        last reading of a long run is the one that matters. It is still worst
        *seen*, not worst possible: a stack cut to its observed peak is a crash
        waiting for a path nothing has taken yet.
        """
        help_text = ("Bytes of stack a task has never used. A high-water mark: "
                     "it only falls, and it is worst seen rather than worst "
                     "possible, so leave margin")
        tightest = fields.get("tightest")
        headroom = _number(fields.get("headroom"))
        if tightest and headroom is not None:
            e.gauge("retimesh_node_stack_tightest_headroom_bytes",
                    "Headroom of the tightest task on the node, and which it is",
                    headroom, dict(base, task=tightest))
        for key, value in sorted(fields.items()):
            if key in _STACKS_META:
                continue
            number = _number(value)
            if number is not None:
                e.gauge("retimesh_node_task_stack_headroom_bytes", help_text,
                        number, dict(base, task=key))
