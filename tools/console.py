#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Talk to a node's maintenance console over TCP.

The console a serial cable carries also answers on port 4243 — every command,
`GET` and `SET` included — so a node can be configured from a distance without
a resident web server (docs/local-link.md). This is a client for it, and a
worked example of the protocol for anything larger.

Usage:
  console.py <host> STATUS                 one command, then exit
  console.py <host> GET radio.sf
  console.py <host>                        interactive
  console.py <host> --password hunter2 …   otherwise it asks, or reads
                                           RETIMESH_PASSWORD

Every caller authenticates: unauthenticated, a session answers HELP, VERSION
and AUTH and nothing else. Three wrong passwords and the node stops listening
to guesses for thirty seconds, so this does not retry.
"""

import argparse
import getpass
import os
import socket
import sys

DEFAULT_PORT = 4243
PREFIX = "RM "


class Console:
    """One session. Replies are read until the OK or ERR line that ends them."""

    def __init__(self, host, port=DEFAULT_PORT, timeout=6.0):
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._buf = b""

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _readline(self):
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("the node closed the connection")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def command(self, line):
        """Send one command; return (ok, [reply lines]).

        A reply is any number of data lines followed by exactly one
        `RM OK …` or `RM ERR …`, so the end is unambiguous and there is
        nothing to guess at. Log lines never begin with the prefix.
        """
        self._sock.sendall((line + "\r\n").encode())
        out = []
        while True:
            got = self._readline()
            if not got.startswith(PREFIX):
                continue                      # log noise sharing the link
            out.append(got)
            body = got[len(PREFIX):]
            if body.startswith("OK ") or body.startswith("ERR "):
                return body.startswith("OK "), out

    def authenticate(self, password):
        ok, reply = self.command("AUTH " + password)
        if not ok:
            raise PermissionError(reply[-1] if reply else "AUTH refused")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("command", nargs="*", help="one command; omit for an interactive session")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--password", default=os.environ.get("RETIMESH_PASSWORD"))
    args = ap.parse_args()

    password = args.password or getpass.getpass("admin password: ")

    try:
        with Console(args.host, args.port) as c:
            c.authenticate(password)
            if args.command:
                ok, reply = c.command(" ".join(args.command))
                print("\n".join(reply))
                return 0 if ok else 1
            print("connected to %s:%d — HELP lists the commands, Ctrl-D to leave"
                  % (args.host, args.port))
            while True:
                try:
                    line = input("> ").strip()
                except EOFError:
                    print()
                    return 0
                if not line:
                    continue
                _, reply = c.command(line)
                print("\n".join(reply))
    except PermissionError as e:
        print("refused: %s" % e, file=sys.stderr)
        return 1
    except (OSError, ConnectionError) as e:
        print("%s:%d: %s" % (args.host, args.port, e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
