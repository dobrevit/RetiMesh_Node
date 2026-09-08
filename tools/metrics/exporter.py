#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Ask a fleet of nodes how they are, and hold the answers for Prometheus.

A node does not volunteer telemetry. It answers a request and says nothing
otherwise — the right design where airtime is the scarce thing, and the reason
a Prometheus job pointed at a mesh collects nothing at all. This is the thing
that asks, and the thing Prometheus scrapes.

It joins the mesh as an ordinary LXMF client, polls each configured node on a
timer, and serves the last answer from each at /metrics. It also keeps
telemetry that arrives unasked — Sideband can be told to send its own, and a
node enrolled against this address may too — so a fleet can be watched either
way round.

    pip install rns lxmf
    python tools/metrics/exporter.py --node <hash>=hilltop --listen :9812

The exporter's own address is printed at startup. Enrol that on each node to
allow the console channel; telemetry needs no enrolment.

Two rates matter and they are not the same rate. Prometheus scrapes every few
seconds; a LoRa node answers every few minutes, if it answers. Everything about
this file follows from that gap — see state.py, which holds the rules for when
a reading stops being publishable.
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import lxmf_wire                                          # noqa: E402
from exposition import CONTENT_TYPE, Exposition           # noqa: E402
from state import Fleet                                   # noqa: E402

DEFAULT_PORT = 9812


class Exporter:
    def __init__(self, args):
        self.args = args
        self.started = time.time()
        self.scrapes = 0
        self.rounds = {"telemetry": 0, "console": 0}
        # One lock over the whole of the state. Three threads reach it: the
        # poll loop, RNS's delivery callback and every HTTP scrape, and a
        # half-updated node rendered mid-scrape is a document Prometheus
        # rejects outright rather than a slightly stale one.
        self.lock = threading.Lock()
        self.fleet = Fleet(max_age=args.max_age, console_max_age=args.console_max_age)

        for address, name in args.node:
            self.fleet.node(address, name=name, polled=True)

        import lxmf_fleet                                   # noqa: E402
        self.client = lxmf_fleet.Client(
            storage=args.storage, rns_config=args.rns_config, peers=args.peer,
            name=args.name, on_message=self.on_message, on_failure=self.on_failure)

        print("exporter address: %s" % self.client.address, flush=True)
        print("enrol that on each node to allow console commands "
              "(telemetry needs no enrolment)", flush=True)

    # --- hearing back ------------------------------------------------------
    def on_message(self, source, telemetry, text):
        now = time.time()
        with self.lock:
            if source not in self.fleet.nodes and not self.args.accept_unsolicited:
                # A stranger's telemetry, and this exporter was told to watch a
                # named fleet. Dropped rather than graphed: on a public mesh
                # anyone can send a readings map, and a series that appears
                # because a passer-by pressed a button is one nobody can
                # explain later.
                if self.args.verbose:
                    print("ignoring telemetry from unconfigured %s" % source, flush=True)
                return
            node = self.fleet.node(source)
            if telemetry is not None:
                readings = lxmf_wire.normalise(telemetry)
                if readings:
                    node.ingest_telemetry(readings, now)
                elif self.args.verbose:
                    print("undecodable telemetry from %s" % source, flush=True)
            if text:
                node.ingest_console(text.splitlines(), now)
        if self.args.verbose:
            print("reply from %s" % source, flush=True)

    def on_failure(self, node_hex, channel, reason, tag=None):
        with self.lock:
            self.fleet.node(node_hex).failed(reason)
        if self.args.verbose:
            print("%s %s: %s (%s)" % (node_hex, channel, reason, tag or "-"), flush=True)

    # --- asking ------------------------------------------------------------
    def ask(self, address, channel, content=""):
        with self.lock:
            self.fleet.node(address).asked(channel)
        self.client.send(address, telemetry=(channel == "telemetry"), content=content)

    # --- being scraped -----------------------------------------------------
    def render(self):
        now = time.time()
        with self.lock:
            self.scrapes += 1
            e = self.fleet.render(now=now)
            e.info("retimesh_exporter_info",
                   "This exporter: the LXMF address to enrol on a node, and the "
                   "library versions it speaks. A client on a different protocol "
                   "to the instance it shares is worse than an old one",
                   {"address": self.client.address,
                    "rns_version": _version("RNS"),
                    "lxmf_version": _version("LXMF")})
            e.gauge("retimesh_exporter_start_timestamp_seconds",
                    "When this exporter started. Readings are held in memory "
                    "only, so a restart is a gap of up to one poll interval",
                    self.started)
            e.counter("retimesh_exporter_scrapes_total",
                      "Scrapes this exporter has served", self.scrapes)
            e.gauge("retimesh_exporter_nodes",
                    "Nodes this exporter is holding readings for",
                    len(self.fleet.nodes))
            for channel, count in sorted(self.rounds.items()):
                e.counter("retimesh_exporter_poll_rounds_total",
                          "Rounds of polling the whole fleet, by channel",
                          count, {"channel": channel})
            return e.render()

    # --- the loop ----------------------------------------------------------
    def run(self):
        stop = threading.Event()

        def bye(*_):
            stop.set()
        signal.signal(signal.SIGINT, bye)
        signal.signal(signal.SIGTERM, bye)

        server = ThreadingHTTPServer(self.args.listen, _handler(self))
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print("serving metrics on http://%s:%d/metrics" % self.args.listen, flush=True)

        # A first pass at once, so a misconfiguration shows up in seconds
        # rather than after the first interval.
        due_telemetry = 0.0
        due_console = 0.0
        due_announce = time.time() + self.args.announce_interval
        while not stop.is_set():
            now = time.time()
            if now >= due_telemetry:
                due_telemetry = now + self.args.interval
                self.rounds["telemetry"] += 1
                for address, _name in self.args.node:
                    self.ask(address, "telemetry")
                    stop.wait(self.args.stagger)
            if self.args.command and now >= due_console:
                due_console = now + self.args.command_interval
                self.rounds["console"] += 1
                for address, _name in self.args.node:
                    for cmd in self.args.command:
                        self.ask(address, "console", cmd)
                        stop.wait(self.args.stagger)
            # Re-announced so a node or a phone that wants to send telemetry
            # unasked can still find this address after it has aged out of
            # somebody's tables. Once at startup is not enough for a process
            # that is meant to run for months.
            if self.args.announce_interval > 0 and now >= due_announce:
                due_announce = now + self.args.announce_interval
                self.client.announce()
            stop.wait(1.0)
        print("stopping", flush=True)
        server.shutdown()


def _version(module):
    try:
        return str(getattr(__import__(module), "__version__", "unknown"))
    except Exception:
        return "unknown"


def _handler(exporter):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):                                   # noqa: N802
            path = self.path.partition("?")[0]
            if path == "/metrics":
                self._say(200, CONTENT_TYPE, exporter.render())
            elif path == "/healthz":
                self._say(200, "text/plain; charset=utf-8", "ok\n")
            elif path == "/":
                self._say(200, "text/html; charset=utf-8", _INDEX)
            else:
                self._say(404, "text/plain; charset=utf-8", "not found\n")

        def _say(self, code, content_type, body):
            raw = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_args):
            # Silent: Prometheus scrapes every few seconds, and a line per
            # scrape buries the one log line that matters — a node answering.
            pass

    return Handler


_INDEX = ("<!doctype html><title>RetiMesh telemetry exporter</title>"
          "<h1>RetiMesh telemetry exporter</h1>"
          "<p>Prometheus metrics for a Reticulum fleet, collected over LXMF.</p>"
          "<p><a href=\"/metrics\">/metrics</a></p>\n")


def _node_spec(text):
    """"<hash>" or "<hash>=<name>" — a node, and optionally what to call it.

    The name is a label on retimesh_node_info and nowhere else, so adding one
    later does not change the identity of any existing series.
    """
    address, _, name = text.partition("=")
    address = address.strip().lower()
    try:
        raw = bytes.fromhex(address)
    except ValueError:
        raise argparse.ArgumentTypeError("not a destination hash: %s" % address)
    if len(raw) != 16:
        print("warning: %s is %d bytes; an LXMF delivery hash is 16"
              % (address, len(raw)), file=sys.stderr, flush=True)
    return address, name.strip()


def _listen(text):
    """"[host]:port", defaulting to every interface on the standard port."""
    host, _, port = text.rpartition(":")
    if not port:
        host, port = text, str(DEFAULT_PORT)
    try:
        return (host or "0.0.0.0", int(port))
    except ValueError:
        raise argparse.ArgumentTypeError("not a [host]:port: %s" % text)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--node", action="append", default=[], type=_node_spec,
                   metavar="HEX[=NAME]",
                   help="an LXMF delivery hash to poll, optionally with a name; "
                        "repeat for each node. Defaults to RETIMESH_NODES, "
                        "comma-separated. Each node reports its own on the "
                        "console: STATUS, the lxmf_address= line")
    p.add_argument("--interval", type=float, default=300.0,
                   help="seconds between telemetry rounds (default 300)")
    p.add_argument("--command", action="append", default=[], metavar="LINE",
                   help="a console line to poll as well, e.g. STACKS; repeatable. "
                        "Defaults to RETIMESH_COMMANDS. Needs this exporter "
                        "enrolled as an administrator on the node")
    p.add_argument("--command-interval", type=float, default=900.0,
                   help="seconds between console rounds (default 900)")
    p.add_argument("--stagger", type=float, default=5.0,
                   help="seconds between nodes, so a round does not put the "
                        "whole fleet on the air at once (default 5)")
    p.add_argument("--max-age", type=float, default=None, metavar="SECONDS",
                   help="stop publishing a node's telemetry this long after its "
                        "last answer, so a node that has died stops drawing a "
                        "flat line (default: three poll intervals). 0 never "
                        "expires, which is only right on a bench")
    p.add_argument("--console-max-age", type=float, default=None, metavar="SECONDS",
                   help="the same, for console readings (default: three console "
                        "intervals)")
    p.add_argument("--listen", type=_listen, default=None, metavar="[HOST]:PORT",
                   help="where to serve /metrics (default :%d, every interface). "
                        "Defaults to RETIMESH_LISTEN" % DEFAULT_PORT)
    p.add_argument("--accept-unsolicited", dest="accept_unsolicited",
                   action="store_true", default=True,
                   help="keep telemetry from nodes that were never configured, "
                        "which is how a Sideband client or a node told to push "
                        "its own readings appears (default)")
    p.add_argument("--only-configured", dest="accept_unsolicited",
                   action="store_false",
                   help="ignore telemetry from anyone not named by --node")
    p.add_argument("--announce-interval", type=float, default=1800.0,
                   help="seconds between announces of this exporter's address, "
                        "so nodes pushing telemetry can still find it (default "
                        "1800; 0 announces only at startup)")
    p.add_argument("--storage", default="/data/rns", help="identity and LXMF state")
    p.add_argument("--rns-config", default="/data/cfg",
                   help="Reticulum config directory; one is written if absent")
    p.add_argument("--peer", action="append", default=[], metavar="HOST[:PORT]",
                   help="connect straight to a node's Reticulum TCP transport "
                        "(default port 4242). Required unless the host's own "
                        "configuration is mounted; repeatable. Defaults to "
                        "RETIMESH_PEERS, comma-separated")
    p.add_argument("--name", default="retimesh exporter", help="LXMF display name")
    p.add_argument("--verbose", action="store_true", help="log every reply and failure")
    args = p.parse_args()

    # Flags win where both are given, so a one-off run can name a single node
    # without editing the file the fleet lives in.
    args.node = args.node or [_node_spec(spec) for spec in lxmf_wire.env_list("RETIMESH_NODES")]
    args.command = args.command or lxmf_wire.env_list("RETIMESH_COMMANDS")
    args.peer = args.peer or lxmf_wire.env_list("RETIMESH_PEERS")
    if args.listen is None:
        args.listen = _listen(os.environ.get("RETIMESH_LISTEN") or ":%d" % DEFAULT_PORT)

    # Derived rather than a fixed default, because the right window is a
    # multiple of how often the thing is asked. Three intervals allows two
    # missed answers before a node's readings are withheld, which is forgiving
    # enough for a mesh and short enough to be honest.
    if args.max_age is None:
        args.max_age = 3 * args.interval
    if args.console_max_age is None:
        args.console_max_age = 3 * args.command_interval

    # An exporter with no nodes is not an error: it still serves, and it still
    # keeps whatever arrives unasked. It is worth saying out loud, though,
    # because the symptom is an empty /metrics that reads as a broken build.
    if not args.node:
        if not args.accept_unsolicited:
            p.error("no --node and --only-configured: this would collect nothing. "
                    "Give at least one node, or set RETIMESH_NODES")
        print("no nodes configured; serving only telemetry that arrives unasked",
              flush=True)

    Exporter(args).run()


if __name__ == "__main__":
    main()
