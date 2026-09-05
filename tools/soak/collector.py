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
here aggregates or graphs: the point is to not lose the readings.

    pip install rns lxmf
    python tools/soak/collector.py --help

Identities are persisted under the storage directory. The collector's own
address is printed at startup, which is what to enrol on the nodes.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import threading
import time

import RNS
import LXMF

# LXMF fields and command ids, from src/rns/LxmfFormat.h and Telemetry.h. They
# are Sideband's numbers rather than ours; the node follows them so that an
# ordinary client can ask the same questions without knowing about this tool.
FIELD_TELEMETRY = 0x02
FIELD_COMMANDS = 0x09
COMMAND_TELEMETRY = 0x01

# Sensor ids inside a telemetry document, and the names this writes them under.
SENSORS = {
    0x01: "time",
    0x02: "location",
    0x04: "battery",
    0x05: "physical_link",
    0x0F: "information",
    0x13: "processor",
    0x14: "ram",
    0x15: "storage",
}


def decode_telemetry(doc):
    """A readings map into something a later reader can use without this file.

    Sensor values keep the shapes the node sends — the arrays are Sideband's,
    not ours, and rewriting them here would be a second definition of the
    format that could drift from the firmware's. The two that are worth naming
    are unpacked because their shape is a nested pair nobody should have to
    remember: processor and RAM are [[label, [capacity, used]]].
    """
    out = {}
    if not isinstance(doc, dict):
        return out
    for sid, value in doc.items():
        name = SENSORS.get(sid, "sensor_0x%02x" % sid)
        if name in ("ram", "processor") and isinstance(value, (list, tuple)) and value:
            try:
                _label, pair = value[0]
                capacity, used = pair
                out[name] = {"capacity": capacity, "used": used,
                             "free": capacity - used}
                continue
            except Exception:
                pass                       # an unexpected shape is kept as it came
        out[name] = value
    return out


def env_list(name):
    """A comma-separated environment variable as a list, blanks discarded.

    Node addresses are the fleet's, not the repository's. Passing them as
    arguments in a compose file means they are committed with it — which is how
    a set of real device addresses ended up in this repository once already.
    An .env file the compose reads and git ignores keeps the deployment's own
    facts out of the deployment's source.
    """
    return [part.strip() for part in os.environ.get(name, "").split(",") if part.strip()]


def seed_config(args):
    """Write a config if there is none, because Reticulum's own default is wrong here.

    Left to itself RNS writes a config with an AutoInterface in it and binds a
    UDP port that, on the machine this runs beside, already belongs to the
    daemon. The container then dies with "address already in use", which reads
    as a broken image rather than as two Reticulum instances colliding.

    What is written instead is a standalone instance whose only interfaces are
    the --peer entries. Standalone is the accurate word and it was not always
    the word used here: an earlier version claimed this attached to the host's
    running instance as a client, and it never did. RNS names its shared-
    instance socket rns/<instance_name>, defaulting to "default", and a host
    that has set instance_name to anything else is simply not found — the
    container starts its *own* shared instance called rns/default and sits
    there with no interfaces at all, reaching nothing. Every "no path yet" in
    early testing was that, misread as the host having no route to the fleet.

    So --peer is not a fallback for an unusual deployment. Without a peer or a
    mounted host configuration this container can reach nothing whatever.

    To genuinely join the host's instance, give it the host's own configuration
    directory (see the compose file): matching the instance name alone is not
    enough, because the shared-instance RPC is authenticated from the identity
    in that directory and a stranger's digest is rejected.
    """
    if not args.rns_config:
        return
    path = os.path.join(args.rns_config, "config")
    if os.path.exists(path):
        return
    os.makedirs(args.rns_config, exist_ok=True)
    lines = ["[reticulum]",
             "  enable_transport = No",
             "  share_instance = Yes",
             "  panic_on_interface_error = No",
             "",
             "[logging]",
             "  loglevel = 3",
             "",
             "[interfaces]"]
    for i, peer in enumerate(args.peer):
        host, _, port = peer.partition(":")
        lines += ["  [[peer%d]]" % i,
                  "    type = TCPClientInterface",
                  "    enabled = yes",
                  "    target_host = %s" % host,
                  "    target_port = %s" % (port or "4242")]
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote a starting config to %s" % path, flush=True)


class Collector:
    def __init__(self, args):
        self.args = args
        self.out_lock = threading.Lock()

        seed_config(args)
        RNS.Reticulum(args.rns_config)
        idpath = os.path.join(args.storage, "identity")
        if os.path.isfile(idpath):
            identity = RNS.Identity.from_file(idpath)
        else:
            os.makedirs(args.storage, exist_ok=True)
            identity = RNS.Identity()
            identity.to_file(idpath)

        self.router = LXMF.LXMRouter(identity=identity, storagepath=args.storage)
        self.local = self.router.register_delivery_identity(
            identity, display_name=args.name)
        self.router.register_delivery_callback(self.on_delivery)
        self.router.announce(self.local.hash)

        print("collector address: %s" % RNS.prettyhexrep(self.local.hash), flush=True)
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
        try:
            dest_hash = bytes.fromhex(node_hex)
        except ValueError:
            print("not a destination hash: %s" % node_hex, file=sys.stderr, flush=True)
            return

        identity = RNS.Identity.recall(dest_hash)
        if identity is None:
            # Without the node's key nothing can be encrypted to it. Asking the
            # network is the whole remedy, and it is worth doing quietly and
            # often rather than once loudly: a node that was asleep at the last
            # attempt answers the next one.
            RNS.Transport.request_path(dest_hash)
            self.record(node_hex, kind, {"error": "no path yet; requested one"})
            return

        dest = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                               "lxmf", "delivery")
        fields = {}
        if kind == "telemetry":
            # An array of one-entry maps, which is the shape every real client
            # sends and the only one the node's parser is written around.
            fields[FIELD_COMMANDS] = [{COMMAND_TELEMETRY: None}]

        msg = LXMF.LXMessage(dest, self.local, content, title="",
                             desired_method=LXMF.LXMessage.DIRECT, fields=fields or None)
        msg.register_failed_callback(
            lambda m: self.record(node_hex, kind, {"error": "delivery failed"}))
        # The request itself, not only the answer. Silence is a result during a
        # soak — a node that was asked and did not reply is a different fact
        # from one that was never asked, and after three days nobody can tell
        # them apart from a file containing only replies.
        self.record(node_hex, kind, {"sent": True})
        self.router.handle_outbound(msg)

    # --- hearing back ------------------------------------------------------
    def on_delivery(self, message):
        src = RNS.hexrep(message.source_hash, delimit=False)
        fields = message.fields or {}
        payload = {}

        if FIELD_TELEMETRY in fields:
            payload["telemetry"] = decode_telemetry(fields[FIELD_TELEMETRY])

        text = ""
        try:
            text = message.content.decode("utf-8", "replace").strip()
        except Exception:
            pass
        if text:
            # A console reply arrives as ordinary text, one "RM ..." line per
            # data row. Kept whole as well as split: the whole is what a human
            # reads and the split is what a script counts.
            payload["text"] = text
            payload["lines"] = [ln for ln in text.splitlines() if ln.strip()]

        if payload:
            self.record(src, "reply", payload)

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
