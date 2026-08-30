# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
retimesh-flash — install RetiMesh Node firmware from GitHub releases.

    retimesh-flash list [--version vX.Y.Z]
    retimesh-flash ports
    retimesh-flash devices [--ip URL ...]
    retimesh-flash bootloader [--port DEV | --serial SN | --ip URL] [--password PW]
    retimesh-flash install [--board ENV] [--port DEV | --serial SN] [--version vX.Y.Z]
                           [--mode full|app|fs] [--file bundle.zip] [--yes] [--no-handoff]

Discovery: <repo>/releases/latest (or /tags/<version>) -> release.json ->
board archive. Every part is SHA-256 checked before esptool runs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from . import device as dev
from .device import DEFAULT_ADMIN, esptool_args, esptool_major, opt  # noqa: F401

DEFAULT_REPO = os.environ.get("RETIMESH_REPO", "dobrevit/RetiMesh_Node")


# ---------------------------------------------------------------------------
# GitHub / download helpers
# ---------------------------------------------------------------------------
def http(url: str) -> bytes:
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json, */*",
        "User-Agent": "retimesh-flash",
        **({"Authorization": f"Bearer {os.environ['GITHUB_TOKEN']}"} if os.environ.get("GITHUB_TOKEN") else {}),
    })
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def fetch_release(repo: str, version: str | None) -> dict:
    which = f"tags/{version}" if version else "latest"
    try:
        return json.loads(http(f"https://api.github.com/repos/{repo}/releases/{which}"))
    except Exception as e:
        sys.exit(f"Cannot fetch release '{version or 'latest'}' from {repo}: {e}")


def fetch_release_json(rel: dict) -> dict:
    assets = {a["name"]: a for a in rel.get("assets", [])}
    if "release.json" not in assets:
        sys.exit(f"Release {rel.get('tag_name')} has no release.json asset (not a firmware release?)")
    return json.loads(http(assets["release.json"]["browser_download_url"]))


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Interaction
# ---------------------------------------------------------------------------
def choose(prompt: str, options: list[tuple[str, str]]) -> str:
    """options: [(value, label)] -> value. Non-interactive with one option."""
    if len(options) == 1:
        return options[0][0]
    if not sys.stdin.isatty():
        sys.exit(f"{prompt}: several candidates and no TTY — pass it explicitly: "
                 + ", ".join(v for v, _ in options))
    print(f"\n{prompt}")
    for i, (_, label) in enumerate(options, 1):
        print(f"  [{i}] {label}")
    while True:
        raw = input(f"Select 1-{len(options)}: ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]


def esp_ports(show_all: bool = False) -> list[tuple[str, str]]:
    """USB serial ports, ESP-looking ones first (one table: device.list_ports).
    Legacy ttyS*/COM ports without a USB id are hidden unless show_all is set."""
    return [(p.device, p.label()) for p in dev.list_ports() if show_all or p.kind != "legacy"]



# ---------------------------------------------------------------------------
# Flashing
# ---------------------------------------------------------------------------

def esptool(args: list[str]) -> None:
    cmd = [sys.executable, "-m", "esptool", *args]
    print("+ " + " ".join(cmd[2:]))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit(f"esptool failed with exit code {rc}")


def flash(board: dict, bundle: Path, port: str, mode: str, baud: int, before: str | None = None) -> None:
    parts = board["parts"]
    if mode == "app":
        parts = [p for p in parts if p["file"] == "firmware.bin"]
    elif mode == "fs":
        parts = [p for p in parts if p["file"] == "littlefs.bin"]

    for p in parts:
        f = bundle / p["file"]
        if not f.exists():
            sys.exit(f"bundle is missing {p['file']}")
        actual = sha256_file(f)
        if actual != p["sha256"]:
            sys.exit(f"checksum mismatch for {p['file']}: expected {p['sha256']}, got {actual}")
    print(f"Verified {len(parts)} part(s).")

    args = esptool_args(board["chip"], port, before or "default_reset", "hard_reset", "write_flash", baud=baud)
    if mode == "full":
        args.append("--erase-all")
    args += [opt("--flash_mode"), board["flash_mode"], opt("--flash_size"), board["flash_size"],
             opt("--flash_freq"), board.get("flash_freq", "80m")]
    for p in parts:
        args += [hex(p["offset"]), str(bundle / p["file"])]
    esptool(args)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_list(args):
    rel = fetch_release(args.repo, args.version)
    rj = fetch_release_json(rel)
    print(f"{rj['firmware']} {rj['version']}  ({rel['html_url']})\n")
    for env, b in rj["boards"].items():
        print(f"  {env:16} {b['name']}")
        if b.get("notes"):
            print(f"  {'':16} {b['notes']}")


def cmd_ports(args):
    ports = esp_ports(args.all)
    if not ports:
        print("No USB serial ports found. Hold BOOT, tap RST, release BOOT, then retry (or --all).")
        return
    for _, label in ports:
        print("  " + label)


def cmd_devices(args):
    """Every RetiMesh node the host can see: serial ports that answer VERSION,
    and any URLs given (or the two well-known addresses) that answer /api/status."""
    from concurrent.futures import ThreadPoolExecutor
    found = 0
    # Ports are independent and a port that is not a node never answers, so
    # each one costs its full timeout: ask them all at once rather than in
    # turn, and print as the answers land.
    ports = [p for p in dev.list_ports() if p.likely_esp or args.all]
    with ThreadPoolExecutor(max_workers=max(1, len(ports))) as pool:
        for p, info in zip(ports, pool.map(lambda p: dev.probe_console(p.device, timeout=1.5), ports)):
            print(f"  {p.label()}" + (f"\n      {info}" if info else "      (no RetiMesh console)"))
            found += bool(info)
    if args.ip:
        urls = [dev.node_url(u) for u in args.ip]
    else:
        # The name is tried only if the address did not answer: resolving
        # retimesh.local on a host with no mDNS resolver can stall for longer
        # than the whole probe, and it can only find the same node.
        urls = ["http://10.42.0.1", "http://retimesh.local"]
    for url in urls:
        info = dev.probe_http(url, timeout=2.0)
        if info:
            print(f"  {info}")
            found += 1
            if not args.ip:
                break                      # the name can only find the same node again
    if not found:
        print("No RetiMesh node answered. Connected over Wi-Fi? Try --ip <address>. On a serial port, "
              "the console needs firmware with the maintenance console (v0.2 or later).")


def cmd_bootloader(args):
    """Put one node into its ROM downloader and say where esptool should point."""
    if args.ip:
        ok, msg, _ = dev.request_bootloader_http(dev.node_url(args.ip), (DEFAULT_ADMIN[0], args.password))
        print(("Requested: " if ok else "Refused: ") + msg)
        sys.exit(0 if ok else 1)
    ports = dev.list_ports()
    port = dev.select_port(ports, device=args.port, serial=args.serial)
    if port is None:
        sys.exit(dev.ambiguous_ports_message(ports, "--port or --serial") if dev.esp_candidates(ports)
                 else "no ESP32 port found")
    r = dev.hand_off_to_bootloader(port=port.device, log=lambda m: print("  " + m), port_hint="--port")
    print(("Downloader ready on " + r.port + f" ({r.method})") if r.entered else r.message)
    # This command promises a downloader. "esptool will reset it" is a fine
    # outcome for `install`, where esptool follows — but nothing follows here,
    # and a script chaining esptool after an exit code of 0 would find the
    # application still running.
    sys.exit(0 if r.entered else 1)


def cmd_install(args):
    with tempfile.TemporaryDirectory(prefix="retimesh-") as tmp:
        tmp = Path(tmp)

        if args.file:
            with zipfile.ZipFile(args.file) as z:
                z.extractall(tmp)
            board = json.loads((tmp / "board.json").read_text())
            print(f"Offline bundle: {board['name']}")
        else:
            rel = fetch_release(args.repo, args.version)
            rj = fetch_release_json(rel)
            boards = rj["boards"]
            env = args.board or choose("Which board are you flashing?",
                                       [(e, f"{b['name']}  [{e}]") for e, b in boards.items()])
            if env not in boards:
                sys.exit(f"unknown board '{env}'; available: {', '.join(boards)}")
            board = boards[env]
            print(f"{rj['firmware']} {rj['version']} → {board['name']}")
            if board.get("notes"):
                print(f"  {board['notes']}")

            archive = tmp / board["archive"]
            print(f"Downloading {board['archive']} …")
            archive.write_bytes(http(board["archive_url"]))
            with zipfile.ZipFile(archive) as z:
                z.extractall(tmp)

        port = args.port
        if not port and args.serial:
            p = dev.select_port(dev.list_ports(), serial=args.serial)
            if p is None:
                sys.exit(f"no single port with USB serial {args.serial}")
            port = p.device
        port = port or choose("Which serial port?", esp_ports() or
                              sys.exit("No serial ports found. Hold BOOT, tap RST, release BOOT, then retry."))

        what = {"full": "FULL install (erases everything, including settings and the web app)",
                "app":  "firmware only (settings and web app are kept)",
                "fs":   "web app filesystem only"}[args.mode]
        print(f"\nAbout to flash: {what}\n  port: {port}\n  chip: {board['chip']}  flash: {board['flash_size']} {board['flash_mode']}")
        if not args.yes and sys.stdin.isatty():
            if input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
                sys.exit("aborted")

        # A running node is asked politely for its downloader first; esptool's
        # own reset remains the fallback, and BOOT+RST the one after that.
        before, node_id = None, None
        if not args.no_handoff:
            r = dev.hand_off_to_bootloader(port=port, log=lambda m: print("  " + m), port_hint="--port")
            before, port, node_id = r.esptool_before, r.port or port, r.node_id
            if not r.entered:
                print("  " + r.message)
                # A hand-off that ends with no port is one whose port has gone:
                # the node was asked, went down, and nothing came back. Running
                # esptool against the path it used to be on fails with an open
                # error that reads like a different fault altogether.
                if r.port is None and not dev.select_port(dev.list_ports(), device=port):
                    sys.exit(f"  {port} is no longer there; nothing to flash")
        flash(board, tmp, port, args.mode, args.baud, before=before)
        wait = dev.application_wait_s(node_id, port)
        info = dev.wait_for_application(port, timeout=wait, node_id=node_id, log=lambda m: print("  " + m))
        print(f"\nBack up: {info}" if info else
              f"\nThe node did not answer within {wait:.0f} s; press RST if it stays quiet.")
        print("\nDone. Join the Wi-Fi network \"retimesh-XXXXXX\" (last six hex digits of the board MAC)\n"
              "and open http://10.42.0.1/")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="retimesh-flash", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub owner/name (default {DEFAULT_REPO})")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="show boards available in a release")
    p.add_argument("--version", help="release tag (default: latest)")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("ports", help="list USB serial ports")
    p.add_argument("--all", action="store_true", help="include ports without a USB id")
    p.set_defaults(func=cmd_ports)

    p = sub.add_parser("devices", help="find RetiMesh nodes on serial ports and the network")
    p.add_argument("--ip", action="append", help="node URL/address to probe (repeatable)")
    p.add_argument("--all", action="store_true", help="probe ports without an ESP-looking USB id too")
    p.set_defaults(func=cmd_devices)

    p = sub.add_parser("bootloader", help="restart a running node into its ROM downloader")
    p.add_argument("--port", help="serial port of the node")
    p.add_argument("--serial", help="USB serial number of the node's port")
    p.add_argument("--ip", help="node URL/address: ask over HTTP instead of the console")
    p.add_argument("--password", default=DEFAULT_ADMIN[1], help="admin password for the HTTP path")
    p.set_defaults(func=cmd_bootloader)

    p = sub.add_parser("install", help="download and flash")
    p.add_argument("--board", help="board env name (see `list`)")
    p.add_argument("--port", help="serial port (auto-detected if omitted)")
    p.add_argument("--serial", help="pick the port by USB serial number")
    p.add_argument("--no-handoff", action="store_true", help="do not ask a running node for its bootloader first")
    p.add_argument("--version", help="release tag (default: latest)")
    p.add_argument("--mode", choices=["full", "app", "fs"], default="full")
    p.add_argument("--file", help="flash from a downloaded retimesh-node-*.zip instead of GitHub")
    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("-y", "--yes", action="store_true", help="do not ask for confirmation")
    p.set_defaults(func=cmd_install)

    args = ap.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
