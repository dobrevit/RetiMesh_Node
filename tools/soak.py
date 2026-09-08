#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Watch a fleet of nodes for as long as it takes, and say what changed.

A soak is only worth running if someone reads the result, and a week of JSON is
not something anyone reads. So this records one row per node per sample, and
prints a summary that answers the questions a soak is actually asked:

  did anything restart, and why      boot count and reset reason, per node
  is memory going anywhere           free heap and its low-water mark, and the
                                     largest block, because fragmentation shows
                                     up in the gap between those two long
                                     before an allocation fails
  is a stack about to overflow       the lowest headroom of any task, by name
  are the tables growing without     paths, links, destinations, announces
    bound
  is traffic being lost, and how     the five loss counters, separately
  can it still hear the channel      the two carrier-sense counters, which are
                                     zero on a healthy node whatever the
                                     traffic, so any value at all is a fault

Every one of those has caught something real on this bench. Nodes are addressed
by their mDNS names, which is what per-node naming was for.

    tools/soak.py --out soak.csv retimesh-8249cc retimesh-d96308
    tools/soak.py --summarise soak.csv

Sampling is deliberately forgiving: a node that misses a poll is recorded as
absent and the run continues. A node that is unreachable for a whole week is a
finding, not a reason to stop collecting from the other three.
"""

import argparse
import csv
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

FIELDS = [
    "ts", "node", "reachable", "uptime_s", "boot_count", "boot_reason", "boot_clean",
    "prev_uptime_s", "heap_free", "heap_min", "heap_largest", "stack_lowest",
    "stack_lowest_task", "rx_packets", "tx_packets", "drop_ring", "drop_reasm",
    "drop_partial", "crc_errors", "bad_length", "spurious_irq", "paths", "links",
    "destinations", "announces", "neighbours", "airtime_long_pct",
    # Liveness, not resources. A node can hold its heap flat and its stacks deep
    # while doing nothing at all: a Heltec Wireless Stick reported "transport:
    # online" for as long as anyone asked while no task was driving Reticulum,
    # and only a person noticing it had not transmitted found it. Resources say
    # whether a node is about to fail; these say whether it is working.
    "transport_online", "announces_tx", "tasks_missing",
    # The byte-addressable heap: what a stack or buffer must come from. The
    # heap_* columns above count 32-bit-only IRAM as well and so read healthy on
    # a board that cannot place another task (Diag.h).
    "dram_free", "dram_min", "dram_largest",
    # Whether the node announces at all. Without it the summary cannot tell a
    # node that is quiet because it was told to be from one that has stopped
    # working.
    "announce_interval",
    # Carrier sense, which is the one part of the radio that fails silently: a
    # probe nobody answers is read as a busy channel, so a node whose CAD has
    # stopped working keeps routing, keeps counting rx and tx, and defers the
    # whole CSMA window before every packet it sends. Nothing else here moves.
    # New columns go at the end, always: appending to a CSV written before they
    # existed leaves every other column where the header says.
    "cad_timeouts", "cad_arm_errors",
    # What has already failed to allocate, and what the node contained when it
    # did. The firmware has recorded these since 2026-09-01 precisely so a soak
    # could say *why* a board restarts rather than only that it did — and this
    # script fetched them in /api/status and threw them away for a week, across
    # three runs and six days of Wireless Bridge panics that went undiagnosed.
    # An allocation failure climbing before a panic is the difference between
    # "it crashed" and "it ran out of byte-addressable DRAM at 06:41".
    "alloc_failures", "faults_contained", "fault_last_ms_ago",
    # The same two counts for the run that *ended*, carried across the restart
    # in RTC memory (BootRecord.h). These are the ones that explain a panic:
    # the columns above are zeroed by the reboot being explained, so on the
    # sample after a restart they describe the few seconds since it, and these
    # describe the hours before it. Blank, never zero, where the RTC domain
    # dropped — a power cut leaves nothing to report and must not read as a
    # clean run.
    "prev_alloc_failures", "prev_contained",
    # The cell, and what the node was doing while it drained it. No board here
    # can measure its own current — the T-Beam's AXP2101 reports voltages and
    # nothing else, the V4's BQ25896 gives charge current but not discharge, and
    # the rest have only a divider — so the only power measurement available
    # without an instrument is how fast the cell falls. That needs the voltage
    # logged over hours, which is what this is for.
    #
    # `power_profile` is not decoration: a discharge figure is meaningless
    # without knowing which profile produced it, and comparing two runs is the
    # entire method. `battery_charging` is the guard — a node on USB is not
    # discharging, and its rising voltage is not a measurement.
    "power_profile", "cpu_mhz", "pmu",
    "battery_present", "battery_v", "battery_pct", "battery_charging",
]

# Every task a healthy node of any board runs. A board without the hardware
# never creates its own (no display, no GPS, no SD), so absence alone is not a
# fault — but these three are on every board and their absence always is.
ALWAYS_RUNNING = ("loopTask", "radio", "rns")


def sample(host, timeout=8):
    row = {f: "" for f in FIELDS}
    row["ts"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    row["node"] = host
    try:
        with urllib.request.urlopen(f"http://{host}.local/api/status", timeout=timeout) as r:
            d = json.load(r)
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        row["reachable"] = 0
        return row

    row["reachable"] = 1
    radio = d.get("radio", {})
    diag = d.get("diag", {})
    boot = diag.get("boot", {})
    heap = diag.get("heap", {})
    tables = diag.get("tables", {})
    faults = diag.get("faults", {})
    power  = d.get("power", {})

    row.update(
        uptime_s=d.get("uptime_s", ""),
        boot_count=boot.get("count", ""),
        boot_reason=boot.get("reason_name", ""),
        boot_clean=int(bool(boot.get("clean"))) if boot else "",
        # Absent, not zero, when a power cut took the RTC domain with it
        prev_uptime_s=boot.get("prev_uptime_s", ""),
        heap_free=heap.get("free", ""),
        heap_min=heap.get("min_free", ""),
        heap_largest=heap.get("largest_block", ""),
        stack_lowest=diag.get("stack_lowest", ""),
        stack_lowest_task=diag.get("stack_lowest_task", ""),
        rx_packets=radio.get("rx_packets", ""),
        tx_packets=radio.get("tx_packets", ""),
        drop_ring=radio.get("rx_dropped_ring", ""),
        drop_reasm=radio.get("rx_dropped_reassembly", ""),
        drop_partial=radio.get("rx_dropped_partial", ""),
        crc_errors=radio.get("rx_crc_errors", ""),
        bad_length=radio.get("rx_bad_length", ""),
        spurious_irq=radio.get("rx_spurious_irq", ""),
        paths=tables.get("paths", ""),
        links=tables.get("links", ""),
        destinations=tables.get("destinations", ""),
        announces=tables.get("announces", ""),
        neighbours=len(d.get("neighbors", [])),
        airtime_long_pct=d.get("airtime", {}).get("long_pct", ""),
        transport_online=int(bool(d.get("transport", {}).get("online"))),
        announces_tx=radio.get("announces_tx", ""),
        announce_interval=radio.get("announce_interval", ""),
        cad_timeouts=radio.get("cad_timeouts", ""),
        cad_arm_errors=radio.get("cad_arm_errors", ""),
        tasks_missing=" ".join(missing_tasks(diag)),
        dram_free=heap.get("dram_free", ""),
        dram_min=heap.get("dram_min_free", ""),
        dram_largest=heap.get("dram_largest_block", ""),
        alloc_failures=faults.get("alloc_failures", ""),
        faults_contained=faults.get("contained", ""),
        # Absent, not zero, when nothing has failed yet: the firmware omits the
        # key until there is a fault to date, and "no failure so far" must not
        # read as "the last one was 0 ms ago".
        fault_last_ms_ago=faults.get("last_ms_ago", ""),
        prev_alloc_failures=boot.get("prev_alloc_failures", ""),
        prev_contained=boot.get("prev_contained", ""),
        power_profile=power.get("profile", ""),
        cpu_mhz=power.get("cpu_mhz", ""),
        pmu=power.get("pmu", ""),
        battery_present=fmt_bool(power.get("battery_present")),
        battery_v=power.get("battery_v", ""),
        battery_pct=power.get("battery_pct", ""),
        # Tri-state on purpose: a board that cannot tell whether it is charging
        # sends null, and that is neither yes nor no. Recording it as "no" would
        # let a run on USB be read as a discharge measurement.
        battery_charging=fmt_bool(power.get("battery_charging")),
    )
    return row


def fmt_bool(v):
    """1, 0, or blank — blank meaning the node did not say, which is a third
    answer and not a false one."""
    if v is True:
        return 1
    if v is False:
        return 0
    return ""


def is_true(v):
    """Decode what fmt_bool() encoded. Paired with it deliberately: the encoding
    is 1/0/blank and the only reader used to re-implement the decode ad hoc,
    which missed a real Python True (str(True) == "True") and read blank —
    "this board cannot tell" — as "no". Both halves live here so they cannot
    disagree about the one distinction this file exists to protect."""
    return v is not None and str(v).strip().lower() in ("1", "true", "yes")


def is_blank(v):
    """None counts. A column a CSV does not have is as absent as one it has and
    left empty, and the difference must not make an old file report findings."""
    return v is None or str(v).strip() == ""


def missing_tasks(diag):
    """Which of the tasks every board runs are not there. `stacks` is a map of
    task name to stack headroom carrying only the tasks that exist — a build
    without a display or a GPS simply has no such key — so a name absent from
    it is a task that is not running."""
    stacks = diag.get("stacks")
    # A node that does not report stacks at all is not evidence of a missing
    # task; say nothing rather than accuse it.
    if not isinstance(stacks, dict) or not stacks:
        return []
    return [t for t in ALWAYS_RUNNING if t not in stacks]


# Hoisted out of summarise() when discharge() came to need them: one definition
# each rather than a second copy beside the first.
def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def parse_ts(v):
    try:
        return datetime.fromisoformat(v).timestamp()
    except (TypeError, ValueError):
        return 0.0


def _crossing(volts, level, start_at=0, confirm=2):
    """Index and interpolated time at which the cell really fell past `level`.

    "Really" is the whole point. A single sample below the level is not a
    crossing: an 18650 sags 100-200 mV under a transmit burst and comes back,
    and load varies constantly on a routing node — so the first sample under a
    threshold is routinely a dip rather than the crossing. `confirm` consecutive
    samples at or below it are required.

    The time is interpolated between the bracketing samples rather than snapped
    to the confirmed one: at a 300 s sample interval, snapping quantises each
    edge by up to five minutes, and both edges of a short band can otherwise
    land inside a single interval and read as zero.

    Returns (index, time) or (None, None).
    """
    for i in range(start_at, len(volts)):
        # `confirm` consecutive samples, except at the end of the series, where
        # there are none left to confirm with. A run deliberately stopped once
        # the cell reached LOW is the normal case and must not read as "never
        # got there"; the cost is that a sag in the final sample cannot be told
        # from a crossing, which is why the caller says so.
        window = min(confirm, len(volts) - i)
        if all(volts[i + k][1] <= level for k in range(window)):
            t, v = volts[i]
            if i == 0:
                return i, t                       # already below at the first sample
            pt, pv = volts[i - 1]
            if pv == v:
                return i, t
            frac = (pv - level) / (pv - v)        # pv > level >= v
            return i, pt + (t - pt) * max(0.0, min(1.0, frac))
    return None, None


def discharge(up, band=None):
    """What the cell did, as lines to print.

    This is the only power measurement available on this bench. No board here
    can report its own current — the T-Beam's AXP2101 exposes voltages and
    nothing else, the V4's BQ25896 gives charge current but not discharge — so
    what is left is how fast the cell falls.

    Read the limits before using a number from this, because it is meant to
    choose a shipped default:

    **mV/h is not proportional to draw across different voltage ranges.**
    dV/dt = (dV/dQ)·I, and dV/dQ for a lithium cell varies by roughly three to
    five times between the 4.2-3.9 V slope, the 3.9-3.7 V plateau and the knee
    below 3.5 V. Two runs that start at different states of charge are not
    comparable in mV/h at all, and whichever run sat in the plateau will look
    like the efficient one. That is why the rate below is reported as an
    observation and `--band` is the figure to compare.

    **Even the band time only compares like with like.** It is ΔQ_band / I_avg,
    and ΔQ_band is constant only for the same physical cell, at a similar
    temperature (ten degrees moves capacity and internal resistance by a few
    percent), at the same state of health — a cell cycled between runs is not
    the same cell — and rested after charging, because surface charge makes a
    high threshold time relaxation rather than discharge.

    **And it measures the node, not the profile.** Everything the board did
    during the run is in the figure. Two runs on a channel with different
    traffic differ for reasons that have nothing to do with the profile, so the
    traffic deltas are printed beside the time: if they do not match, the runs
    were not comparable and the difference is not the profile's.

    Four things make a run outright invalid rather than merely incomparable,
    and each is reported instead of being folded into a number: the node was
    charging, it cannot tell whether it was charging, its profile changed
    mid-run, or it has no cell to measure.
    """
    out = []

    # A cell the board actually reports. `battery_v` alone is not enough: an
    # ADC board leaves the last reading in place when the converter stops, and
    # the API publishes it regardless of `present`, so a stale divider would
    # otherwise yield a confident rate.
    rows = [r for r in up if is_true(r.get("battery_present"))]
    if not rows:
        if any(not is_blank(r.get("battery_present")) for r in up):
            out.append("   this board reports no cell — nothing to measure this way")
        return out                              # or a CSV predating the columns

    volts = [(parse_ts(r["ts"]), num(r.get("battery_v"))) for r in rows]
    volts = [(t, v) for t, v in volts if t and v]
    if len(volts) < 2:
        return out

    # Asked before the run is split, because a charging node's voltage *rises*
    # and the split below reads a rise as a recharge — which would file the
    # whole run away as several one-sample stretches and report nothing at all,
    # when what the operator needs is to be told they measured a charger.
    if any(is_true(r.get("battery_charging")) for r in rows):
        out.append("   ⚠ the node was charging during this run: this is not a "
                   "discharge measurement")
        return out

    # One discharge, not several. The CSV is appended to by design, so a file
    # can hold last week's run, a recharge and today's; and a restart can be a
    # node that was replugged, which is a different draw. Split on both and
    # measure the longest clean stretch, the way announce_segments() splits the
    # announce counter at every boot.
    segs, seg = [], [0]
    for i in range(1, len(volts)):
        recharged = volts[i][1] > volts[i - 1][1] + 0.030      # 30 mV up: not noise
        rebooted = rows[i].get("boot_count") != rows[i - 1].get("boot_count")
        if recharged or rebooted:
            segs.append(seg); seg = [i]
        else:
            seg.append(i)
    segs.append(seg)
    idx = max(segs, key=lambda g: (volts[g[-1]][0] - volts[g[0]][0]) if len(g) > 1 else -1)
    if len(idx) < 2:
        return out
    if len(segs) > 1:
        out.append(f"   the file holds {len(segs)} discharge stretches (recharges or "
                   f"restarts between them); measuring the longest")
    volts = [volts[i] for i in idx]
    rows = [rows[i] for i in idx]

    charging = [r.get("battery_charging") for r in rows]
    profiles = {r.get("power_profile") for r in rows if r.get("power_profile")}
    first_t, first_v = volts[0]
    last_t, last_v = volts[-1]
    hours = (last_t - first_t) / 3600.0
    prof = "/".join(sorted(profiles)) if profiles else "unknown"
    out.append(f"   battery {first_v:.3f} V -> {last_v:.3f} V over {hours:.2f} h"
               f"  (profile {prof})")

    invalid = False
    if all(is_blank(c) for c in charging):
        # Blank is the third answer, not "no". Only the T-Beam and the V4 can
        # tell; everywhere else the API sends null and a run on USB would
        # otherwise be reported as a discharge.
        out.append("   ⚠ this board cannot tell whether it was charging — confirm "
                   "by hand that it was on battery before using this")
    if len(profiles) > 1:
        out.append(f"   ⚠ the power profile changed during this run ({prof}): this "
                   f"measures neither profile")
        invalid = True
    if invalid:
        return out
    if hours <= 0:
        return out

    # A hole in the samples is time the node was not observed, and attributing
    # it to the profile is how an unreachable node becomes an efficient one.
    gaps = [volts[i][0] - volts[i - 1][0] for i in range(1, len(volts))]
    typical = sorted(gaps)[len(gaps) // 2]
    if gaps and max(gaps) > max(4 * typical, 900) and max(gaps) > 0.1 * (last_t - first_t):
        out.append(f"   ⚠ the largest gap between samples is {max(gaps) / 3600.0:.2f} h "
                   f"of a {hours:.2f} h run — the node was not observed for much of it")

    drop_mv = (first_v - last_v) * 1000.0
    if drop_mv <= 0:
        out.append("   the cell did not fall over this run — too short, or not on battery")
        return out
    out.append(f"   fell {drop_mv:.0f} mV in {hours:.2f} h ({drop_mv / hours:.1f} mV/h) — "
               f"comparable only against a run over the same voltage range")

    if not band:
        return out
    hi, lo = band
    if volts[0][1] <= hi:
        out.append(f"   band {hi:.3f}-{lo:.3f} V: the run began at {volts[0][1]:.3f} V, "
                   f"already at or below {hi:.3f} — it never entered the band")
        return out
    i_hi, t_hi = _crossing(volts, hi)
    if i_hi is None:
        out.append(f"   band {hi:.3f}-{lo:.3f} V: never fell past {hi:.3f} V")
        return out
    i_lo, t_lo = _crossing(volts, lo, start_at=i_hi)
    if i_lo is None:
        out.append(f"   band {hi:.3f}-{lo:.3f} V: not fully covered — the run ended at "
                   f"{last_v:.3f} V")
        return out
    span = t_lo - t_hi
    if span < 3 * typical:
        out.append(f"   band {hi:.3f}-{lo:.3f} V crossed inside {span / 3600.0:.2f} h, "
                   f"which is fewer than three sample intervals: too fast to resolve at "
                   f"this --interval, not a measurement")
        return out
    out.append(f"   band {hi:.3f}-{lo:.3f} V crossed in {span / 3600.0:.2f} h "
               f"— compare against another run of the same cell")
    # Whether the two runs were comparable at all is not visible from the time.
    traffic = []
    for label, key in (("rx", "rx_packets"), ("tx", "tx_packets"),
                       ("announces", "announces_tx")):
        a, b = num(rows[0].get(key)), num(rows[-1].get(key))
        if a is not None and b is not None and b >= a:
            traffic.append(f"{label} +{b - a:.0f}")
    if traffic:
        out.append(f"   over that band the node did: {', '.join(traffic)} — a run with "
                   f"different traffic is not a comparison of profiles")
    return out


def summarise(path, band=None):
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f)]
    if not rows:
        print("no samples")
        return

    def announce_segments(rows):
        """The announce counter, split at every restart, over the samples that
        actually carry it. It is a RAM counter and starts again at zero on
        every boot, so a run spanning one holds several counters rather than
        one long one; and a sample that does not report it (a node running
        firmware from before the column existed) says nothing about any
        window, so it is not part of one."""
        segments, current, boot = [], [], object()
        for r in rows:
            if r.get("boot_count", "") != boot:
                boot = r.get("boot_count", "")
                current = []
                segments.append(current)
            v = num(r.get("announces_tx"))
            if v is not None:
                current.append((parse_ts(r["ts"]), v))
        return [s for s in segments if s]

    for node in sorted({r["node"] for r in rows}):
        rs = [r for r in rows if r["node"] == node]
        up = [r for r in rs if r["reachable"] == "1"]
        print(f"══ {node}   {len(up)}/{len(rs)} samples reachable")
        if not up:
            print("   never answered")
            continue

        first, last = up[0], up[-1]
        print(f"   first {first['ts']}   last {last['ts']}")

        # Restarts. A rising boot count is the headline; the reason says what to
        # think about it, and prev_uptime_s says how long the run before lasted.
        boots = sorted({r["boot_count"] for r in up if r["boot_count"]})
        if len(boots) > 1:
            print(f"   RESTARTED during the run: boot {boots[0]} -> {boots[-1]}")
            for a, b in zip(up, up[1:]):
                if a["boot_count"] == b["boot_count"]:
                    continue
                prev = b["prev_uptime_s"] or "unknown (power lost)"
                print(f"     {b['ts']}  reason={b['boot_reason']}  previous run {prev}s")
                # What that dead run had already failed to allocate, said here
                # rather than in its own pass: one restart is one event, and
                # splitting it across two loops put the reason and the
                # explanation in different paragraphs with a range summary
                # between them. Reported per restart, never as a range — each
                # value belongs to one dead run and a min/max would blur the
                # one that matters.
                pa = num(b.get("prev_alloc_failures"))
                pc = num(b.get("prev_contained"))
                if pa is None:
                    continue      # a CSV from before the columns, or nothing survived
                if pa:
                    print(f"       ⚠ it had {int(pa)} allocation failure(s) before it "
                          f"stopped — it died short of memory")
                if pc:
                    print(f"       it contained {int(pc)} exception(s)")
                if not pa and not pc:
                    print("       it reported no allocation failures")
        else:
            print(f"   no restarts (boot #{boots[0] if boots else '?'}), "
                  f"uptime {last['uptime_s']}s")

        # What failed to allocate while it was up. This sits with the restarts
        # rather than with the heap figures because it is the bridge between
        # them: free heap says how close a node is to the edge, and this says
        # how many times it has already gone over. A count that climbs across a
        # boot is the same pressure surviving the reboot.
        allocs = [n for n in (num(r.get("alloc_failures")) for r in up) if n is not None]
        caught = [n for n in (num(r.get("faults_contained")) for r in up) if n is not None]
        if allocs and max(allocs) > 0:
            print(f"   ⚠ ALLOCATION FAILURES: {int(min(allocs))} -> {int(max(allocs))} during the run"
                  + (f", {int(max(caught))} contained" if caught else ""))
        elif allocs:
            print("   no allocation failures")
        # A CSV written before these columns existed says nothing at all here,
        # and silence is the honest report — not "none".

        # Liveness first, and loudly. Everything below this says whether a node
        # is heading for trouble; this says whether it is doing its job at all,
        # which is the failure that hides behind healthy resource figures.
        missing = sorted({t for r in up for t in (r.get("tasks_missing") or "").split() if t})
        if missing:
            worst = max(sum(1 for r in up if t in (r.get("tasks_missing") or "").split())
                        for t in missing)
            print(f"   ⚠ TASK MISSING: {', '.join(missing)} — absent in up to "
                  f"{worst}/{len(up)} samples; this node is not doing that work")
        offline = [r for r in up if r.get("transport_online") == "0"]
        if offline:
            print(f"   ⚠ transport offline in {len(offline)}/{len(up)} samples")
        # Announcing switched off is a setting the settings page permits, not a
        # fault, and a summary that calls it a failure teaches the operator to
        # skip the warning. A CSV from before the column existed says nothing
        # either way, and then the check runs as it always did.
        intervals = {num(r.get("announce_interval")) for r in up
                     if num(r.get("announce_interval")) is not None}
        segments = announce_segments(up)
        if intervals == {0.0}:
            print("   announces: switched off (announce_interval 0)")
        elif segments:
            # From the timestamps of the samples that carry the counter, not
            # the sample count and not the whole recording: the interval is an
            # argument, and a stated duration that assumes it would be wrong.
            stuck = [s for s in segments if s[-1][1] == s[0][1] and s[-1][0] - s[0][0] >= 3600]
            for s in stuck:
                print(f"   ⚠ announced nothing in {(s[-1][0] - s[0][0]) / 3600.0:.0f}h "
                      f"(announces_tx stuck at {s[0][1]:.0f}) — a node with announcing on "
                      "should announce every ANNOUNCE_INTERVAL_S")
            if not stuck:
                if len(segments) == 1:
                    print(f"   announces sent: {segments[0][0][1]:.0f} -> {segments[0][-1][1]:.0f}")
                else:
                    sent = sum(s[-1][1] - s[0][1] for s in segments)
                    print(f"   announces sent: {sent:.0f} over {len(segments)} boots")

        # Memory. The trend matters more than any single reading.
        fr = [num(r["heap_free"]) for r in up if num(r["heap_free"]) is not None]
        lg = [num(r["heap_largest"]) for r in up if num(r["heap_largest"]) is not None]
        mn = [num(r["heap_min"]) for r in up if num(r["heap_min"]) is not None]
        if fr and lg:
            print(f"   heap free {fr[0]/1024:.0f}K -> {fr[-1]/1024:.0f}K "
                  f"(min seen {min(mn)/1024:.0f}K), largest block "
                  f"{lg[0]/1024:.0f}K -> {lg[-1]/1024:.0f}K, "
                  f"fragmentation {(fr[-1]-lg[-1])/1024:.0f}K")

        # The byte-addressable heap, which is the one a task stack comes from:
        # a node can look healthy on the line above and be unable to place one.
        df = [num(r.get("dram_free")) for r in up if num(r.get("dram_free")) is not None]
        dl = [num(r.get("dram_largest")) for r in up if num(r.get("dram_largest")) is not None]
        if df and dl:
            print(f"   dram free {df[0]/1024:.0f}K -> {df[-1]/1024:.0f}K, "
                  f"largest block {dl[0]/1024:.0f}K -> {dl[-1]/1024:.0f}K"
                  + ("   ⚠ under 16K: the rns stack would not fit today" if dl[-1] < 16384 else ""))

        st = [(num(r["stack_lowest"]), r["stack_lowest_task"]) for r in up if num(r["stack_lowest"])]
        if st:
            worst = min(st, key=lambda x: x[0])
            print(f"   lowest stack headroom seen: {worst[0]:.0f} B on \"{worst[1]}\"")

        for label, key in (("paths", "paths"), ("links", "links"),
                           ("destinations", "destinations"), ("announces", "announces")):
            vals = [num(r[key]) for r in up if num(r[key]) is not None]
            if vals and (vals[0] != vals[-1] or max(vals) != vals[-1]):
                print(f"   {label}: {vals[0]:.0f} -> {vals[-1]:.0f} (peak {max(vals):.0f})")

        for line in discharge(up, band):
            print(line)

        # Losses, as deltas: totals since boot say little a week in.
        loss_keys = ("drop_ring", "drop_reasm", "drop_partial", "crc_errors",
                     "bad_length", "spurious_irq")
        deltas = {}
        for k in loss_keys:
            a, b = num(first[k]), num(last[k])
            if a is not None and b is not None and b >= a and b - a > 0:
                deltas[k] = b - a
        rx = (num(last["rx_packets"]) or 0) - (num(first["rx_packets"]) or 0)
        print(f"   rx +{rx:.0f}  losses " + (str({k: int(v) for k, v in deltas.items()})
                                             if deltas else "none"))

        # Carrier sense, and not as a delta. The counters above are a matter of
        # degree — a channel with weather on it drops frames and the question is
        # how many more than last week — but on a healthy node these two are
        # zero whatever the traffic, so the value itself is the finding and a
        # run that starts at a hundred and ends at a hundred is still a broken
        # node. Reported per sample rather than first-to-last for the same
        # reason: a fault that stopped when the node last restarted still
        # happened.
        #
        # The two say different things and send you to different places, which
        # is why they get a sentence each: a timeout is a probe that was armed
        # and never reported — usually the interrupt line — while an arm error
        # is the driver refusing to start one, and RadioLib has already said why
        # in the log. Both are read as a busy channel, so the symptom on the air
        # is identical and the thing to check is not (docs/troubleshooting.md).
        cad_meaning = {
            "cad_timeouts":
                "the carrier-sense probe is not being answered, so every packet this node "
                "sends waits out the whole CSMA deferral — check the DIO the scan uses",
            "cad_arm_errors":
                "the driver would not arm carrier sense at all, which is a RadioLib refusal "
                "rather than wiring — the log carries the code; a wedged part looks like this",
        }
        for key, meaning in cad_meaning.items():
            vals = [v for v in (num(r.get(key)) for r in up) if v is not None]
            if not vals or max(vals) == 0:
                continue
            print(f"   ⚠ {key}: {vals[0]:.0f} -> {vals[-1]:.0f} (peak {max(vals):.0f}) — "
                  f"{meaning} (docs/troubleshooting.md)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nodes", nargs="*", help="mDNS names, without .local")
    ap.add_argument("--out", default="soak.csv", help="CSV to append to")
    ap.add_argument("--interval", type=int, default=300, help="seconds between samples")
    ap.add_argument("--duration", type=float, default=7 * 24 * 3600,
                    help="seconds to run for (default: a week)")
    ap.add_argument("--summarise", metavar="CSV",
                    help="print a summary of an existing file and exit")
    ap.add_argument("--band", metavar="HIGH:LOW",
                    help="with --summarise: time the cell's fall through this "
                         "voltage window, e.g. 4.00:3.80. The figure to compare "
                         "between two runs of the same cell — see the caveats "
                         "discharge() prints, which are not optional reading")
    args = ap.parse_args()

    band = None
    if args.band:
        try:
            hi, lo = (float(x) for x in args.band.split(":", 1))
        except ValueError:
            ap.error("--band wants HIGH:LOW in volts, e.g. 4.00:3.80")
        if hi <= lo:
            ap.error(f"--band {args.band}: a cell falls, so HIGH must exceed LOW")
        if not args.summarise:
            ap.error("--band is only meaningful with --summarise")
        band = (hi, lo)

    if args.summarise:
        summarise(args.summarise, band)
        return 0
    if not args.nodes:
        ap.error("name at least one node, or pass --summarise")

    # Append, so a run interrupted and restarted keeps its history.
    import os
    new = not os.path.exists(args.out) or os.path.getsize(args.out) == 0
    end = time.time() + args.duration
    with open(args.out, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if new:
            w.writeheader()
        while time.time() < end:
            for n in args.nodes:
                w.writerow(sample(n))
            f.flush()
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
