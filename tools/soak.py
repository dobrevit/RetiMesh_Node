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
]


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
    )
    return row


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

        # Memory. The trend matters more than any single reading.
        fr = [num(r["heap_free"]) for r in up if num(r["heap_free"]) is not None]
        lg = [num(r["heap_largest"]) for r in up if num(r["heap_largest"]) is not None]
        mn = [num(r["heap_min"]) for r in up if num(r["heap_min"]) is not None]
        if fr and lg:
            print(f"   heap free {fr[0]/1024:.0f}K -> {fr[-1]/1024:.0f}K "
                  f"(min seen {min(mn)/1024:.0f}K), largest block "
                  f"{lg[0]/1024:.0f}K -> {lg[-1]/1024:.0f}K, "
                  f"fragmentation {(fr[-1]-lg[-1])/1024:.0f}K")

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
