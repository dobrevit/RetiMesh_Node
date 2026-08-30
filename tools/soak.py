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
    # working. Last in the list on purpose: appending to a CSV written before
    # this column existed leaves every other column where the header says.
    "announce_interval",
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
        tasks_missing=" ".join(missing_tasks(diag)),
        dram_free=heap.get("dram_free", ""),
        dram_min=heap.get("dram_min_free", ""),
        dram_largest=heap.get("dram_largest_block", ""),
    )
    return row


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


def summarise(path):
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f)]
    if not rows:
        print("no samples")
        return

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
                if a["boot_count"] != b["boot_count"]:
                    prev = b["prev_uptime_s"] or "unknown (power lost)"
                    print(f"     {b['ts']}  reason={b['boot_reason']}  previous run {prev}s")
        else:
            print(f"   no restarts (boot #{boots[0] if boots else '?'}), "
                  f"uptime {last['uptime_s']}s")

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
    args = ap.parse_args()

    if args.summarise:
        summarise(args.summarise)
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
