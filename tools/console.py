#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Talk to a node's maintenance console, over the cable or over the network.

The console a serial port carries also answers on TCP port 4243 — every
command, `GET` and `SET` included — so a node can be configured from a
distance without a resident web server (docs/local-link.md). The protocol is
the same on both, so this takes either and works out which from what it is
given: a path or a COM name is a port, anything else is a host.

  console.py /dev/ttyUSB0 STATUS          over the cable
  console.py 192.168.1.50 STATUS          over the network
  console.py retimesh-52a7f8.local:4243   ...on another port
  console.py /dev/ttyUSB0                 interactive
  console.py 192.168.1.50 SET radio.sf 9

A network session authenticates first — the cable does not, because physical
access is already more than a password. The password comes from --password,
then RETIMESH_PASSWORD, then a prompt. Wrong ones are counted by the node,
which stops answering AUTH for a while after a few, so this never retries.

The protocol reader is retimesh_flash.device.Console, the same one the
flashing tool uses: one client, two transports.
"""

import argparse
import getpass
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "retimesh-flash"))

try:
    from retimesh_flash import device
except ImportError as exc:                     # pyserial missing, or a moved tree
    print("cannot load retimesh_flash (%s) — pip install pyserial" % exc, file=sys.stderr)
    sys.exit(1)


def open_console(target: str, timeout: float):
    """A session on whichever transport `target` names."""
    if device.is_device_path(target):
        return device.Console.open(target, timeout=timeout)
    host, port = device.split_host_port(target)
    return device.Console.connect(host, port, timeout=timeout)


def run(console, line: str) -> int:
    """One command. Prints what the node said, in its own words."""
    status, _, _ = console.command(line)
    if console.last_lines:
        print("\n".join(console.last_lines))
    if status == "TIMEOUT":
        print("no reply within %.0fs" % console.timeout, file=sys.stderr)
        return 1
    if status == "SHORT":
        print("the reply was short of the lines it announced", file=sys.stderr)
        return 1
    return 0 if status == "OK" else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="serial port (/dev/ttyUSB0, COM7) or host[:port]")
    ap.add_argument("command", nargs="*", help="one command; omit for an interactive session")
    ap.add_argument("--password", default=os.environ.get("RETIMESH_PASSWORD"),
                    help="admin password; network sessions only")
    ap.add_argument("--timeout", type=float, default=4.0)
    args = ap.parse_args()

    try:
        console = open_console(args.target, args.timeout)
    except Exception as exc:                   # a missing port and a refused socket read alike
        print("%s: %s" % (args.target, exc), file=sys.stderr)
        return 1

    try:
        if console.networked:
            password = args.password or getpass.getpass("admin password: ")
            ok, why = console.authenticate(password)
            if not ok:
                print("%s: %s" % (args.target, why), file=sys.stderr)
                return 1
        elif args.password:
            # Sending it anyway would be worse than useless: the cable is
            # already trusted, and a typo there is a wrong password the node
            # counts against whoever tries next over the network.
            print("(the cable needs no password; --password ignored)", file=sys.stderr)

        if args.command:
            return run(console, " ".join(args.command))

        print("%s — HELP lists the commands, Ctrl-D to leave" % console.device)
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if line:
                run(console, line)
    finally:
        console.close()


if __name__ == "__main__":
    sys.exit(main())
