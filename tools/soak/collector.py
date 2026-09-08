#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Ask a fleet of nodes how they are, on a timer, and keep every answer.

A node does not volunteer telemetry. It answers a request and says nothing
otherwise — which is the right design for a mesh where airtime is the scarce
thing, and it means a soak test with nobody asking produces no data at all.
This is the thing that asks.

Two channels, because the interesting numbers are split across them:

  * A telemetry request (LXMF FIELD_COMMANDS 0x09, command 0x01) is open to
    any sender and comes back as a readings map — clock, battery, position,
    signal, processor, RAM, storage. RAM here is the node's *internal* memory,
    not the total including PSRAM, which is the figure that decides whether a
    board survives.

  * A console line sent as ordinary message text reaches the same parser the
    cable uses, if this collector's identity is enrolled as an administrator on
    the node. That is how STACKS is asked, and per-task stack headroom is the
    measurement a soak exists to take: it only falls, so days of real traffic
    are worth more than any bench session.

Everything lands in newline-delimited JSON, one object per answer, so a run
that is interrupted keeps what it already had and a later run appends. Nothing
here aggregates or graphs: the point is to not lose the readings. For the same
fleet turned into gauges a dashboard can draw, see tools/metrics/, which asks
the same questions over the same client and keeps only the last answer.

    pip install rns lxmf
    python tools/soak/collector.py --help

Identities are persisted under the storage directory. The collector's own
address is printed at startup, which is what to enrol on the nodes.

The radio work is lxmf_fleet.py and the wire formats are lxmf_wire.py; both are
shared with the exporter, so what a reading *is* has one definition rather than
one per tool.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import lxmf_wire                                          # noqa: E402
from lxmf_wire import env_list                            # noqa: E402

# What a failure is called in the file. The words are older than the reasons
# lxmf_fleet reports and are kept as they are: a soak that has been running
# since before this was split apart is still being read with jq expressions
# that match on them.
FAILURES = {
    "no_path": "no path yet; requested one",
    "delivery_failed": "delivery failed",
}


class Collector:
    def __init__(self, args):
        self.args = args
        self.out_lock = threading.Lock()

        import lxmf_fleet                                  # noqa: E402
        self.client = lxmf_fleet.Client(
            storage=args.storage, rns_config=args.rns_config, peers=args.peer,
            name=args.name, on_message=self.on_delivery, on_failure=self.on_failure)

        print("collector address: <%s>" % self.client.address, flush=True)
        print("enrol that on each node to allow console commands "
              "(telemetry needs no enrolment)", flush=True)

    # --- writing -----------------------------------------------------------
    def record(self, node, kind, payload):
        row = {"at": time.time(), "node": node, "kind": kind}
        row.update(payload)
        line = json.dumps(row, default=str)
        with self.out_lock:
            with open(self.args.out, "a") as fh:
                fh.write(line + "\n")
                fh.flush()
        if self.args.verbose:
            print(line, flush=True)

    # --- asking ------------------------------------------------------------
    def send(self, node_hex, kind, content=""):
        # The request itself, not only the answer. Silence is a result during a
        # soak — a node that was asked and did not reply is a different fact
        # from one that was never asked, and after three days nobody can tell
        # them apart from a file containing only replies.
        if self.client.send(node_hex, telemetry=(kind == "telemetry"),
                            content=content, tag=kind):
            self.record(node_hex, kind, {"sent": True})

    def on_failure(self, node_hex, channel, reason, tag):
        if reason == "bad_address":
            return                          # already said on stderr, and not a reading
        self.record(node_hex, tag or channel, {"error": FAILURES.get(reason, reason)})

    # --- hearing back ------------------------------------------------------
    def on_delivery(self, source, telemetry, text):
        payload = {}
        if telemetry is not None:
            payload["telemetry"] = lxmf_wire.decode_telemetry(telemetry)
        if text:
            # A console reply arrives as ordinary text, one "RM ..." line per
            # data row. Kept whole as well as split: the whole is what a human
            # reads and the split is what a script counts.
            payload["text"] = text
            payload["lines"] = [ln for ln in text.splitlines() if ln.strip()]
        if payload:
            self.record(source, "reply", payload)

    # --- the loop ----------------------------------------------------------
    def run(self):
        stop = threading.Event()

        def bye(*_):
            stop.set()
        signal.signal(signal.SIGINT, bye)
        signal.signal(signal.SIGTERM, bye)

        # A first pass at once, so a misconfiguration shows up in seconds
        # rather than after the first interval.
        due_telemetry = 0.0
        due_console = 0.0
        while not stop.is_set():
            now = time.time()
            if now >= due_telemetry:
                due_telemetry = now + self.args.interval
                for node in self.args.node:
                    self.send(node, "telemetry")
                    stop.wait(self.args.stagger)
            if self.args.command and now >= due_console:
                due_console = now + self.args.command_interval
                for node in self.args.node:
                    for cmd in self.args.command:
                        self.send(node, "console:" + cmd, cmd)
                        stop.wait(self.args.stagger)
            stop.wait(1.0)
        print("stopping", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--node", action="append", default=[], metavar="HEX",
                   help="an LXMF delivery hash to poll; repeat for each node. "
                        "Defaults to SOAK_NODES, comma-separated")
    p.add_argument("--interval", type=float, default=300.0,
                   help="seconds between telemetry rounds (default 300)")
    p.add_argument("--command", action="append", default=[], metavar="LINE",
                   help="a console line to send as well, e.g. STACKS; repeatable. "
                        "Defaults to SOAK_COMMANDS, comma-separated. Needs this "
                        "collector enrolled as an administrator")
    p.add_argument("--command-interval", type=float, default=900.0,
                   help="seconds between console rounds (default 900)")
    p.add_argument("--stagger", type=float, default=5.0,
                   help="seconds between nodes, so a round does not put the "
                        "whole fleet on the air at once (default 5)")
    p.add_argument("--out", default="/data/soak.jsonl", help="newline-delimited JSON output")
    p.add_argument("--storage", default="/data/rns", help="identity and LXMF state")
    p.add_argument("--rns-config", default="/data/cfg",
                   help="Reticulum config directory; one is written if absent")
    p.add_argument("--peer", action="append", default=[], metavar="HOST[:PORT]",
                   help="connect straight to a node\'s Reticulum TCP transport "
                        "(default port 4242). Use where the host instance has no "
                        "route to the fleet; repeatable. Defaults to SOAK_PEERS, "
                        "comma-separated")
    p.add_argument("--name", default="soak collector", help="LXMF display name")
    p.add_argument("--verbose", action="store_true", help="echo every row as it lands")
    args = p.parse_args()

    # Flags win where both are given, so a one-off run can name a single node
    # without editing the file the fleet lives in.
    args.node = args.node or env_list("SOAK_NODES")
    args.command = args.command or env_list("SOAK_COMMANDS")
    args.peer = args.peer or env_list("SOAK_PEERS")

    if not args.node:
        p.error("give at least one --node, or set SOAK_NODES to a comma-separated list")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    Collector(args).run()


if __name__ == "__main__":
    main()
