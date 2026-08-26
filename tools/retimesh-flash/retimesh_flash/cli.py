# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
retimesh-flash — install RetiMesh Node firmware from GitHub releases.

    retimesh-flash list [--version vX.Y.Z]
    retimesh-flash ports
    retimesh-flash install [--board ENV] [--port DEV] [--version vX.Y.Z]
                           [--mode full|app|fs] [--file bundle.zip] [--yes]

Discovery: <repo>/releases/latest (or /tags/<version>) -> release.json ->
board archive. Every part is SHA-256 checked before esptool runs.
"""
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

DEFAULT_REPO = os.environ.get("RETIMESH_REPO", "dobrevit/RetiMesh_Node")
ESP_USB_VIDS = {0x303A, 0x10C4, 0x1A86, 0x0403}   # Espressif, CP210x, CH34x, FTDI


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
    """USB serial ports, ESP-looking vendor IDs first. Legacy ttyS*/COM
    ports without a USB id are hidden unless show_all is set."""
    from serial.tools import list_ports
    found = []
    for p in list_ports.comports():
        if p.vid is None and not show_all:
            continue
        likely = p.vid in ESP_USB_VIDS
        tag = "" if likely else "  (unknown USB vendor)" if p.vid is not None else "  (no USB id)"
        found.append((p.device, f"{p.device} — {p.description}{tag}", likely))
    found.sort(key=lambda t: not t[2])
    return [(d, l) for d, l, _ in found]


# ---------------------------------------------------------------------------
# Flashing
# ---------------------------------------------------------------------------
def esptool_major() -> int:
    try:
        import esptool
        return int(esptool.__version__.split(".")[0])
    except Exception:
        return 4


def opt(name: str) -> str:
    """esptool >= 5 spells options and commands with dashes; 4.x only
    accepts underscores. Emit whichever the installed version wants."""
    return name.replace("_", "-") if esptool_major() >= 5 else name


def esptool(args: list[str]) -> None:
    cmd = [sys.executable, "-m", "esptool", *args]
    print("+ " + " ".join(cmd[2:]))
    rc = subprocess.call(cmd)
    if rc != 0:
        sys.exit(f"esptool failed with exit code {rc}")


def flash(board: dict, bundle: Path, port: str, mode: str, baud: int) -> None:
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

    args = ["--chip", board["chip"], "--port", port, "--baud", str(baud),
            "--before", opt("default_reset"), "--after", opt("hard_reset"), opt("write_flash")]
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

        port = args.port or choose("Which serial port?", esp_ports() or
                                   sys.exit("No serial ports found. Hold BOOT, tap RST, release BOOT, then retry."))

        what = {"full": "FULL install (erases everything, including settings and the web app)",
                "app":  "firmware only (settings and web app are kept)",
                "fs":   "web app filesystem only"}[args.mode]
        print(f"\nAbout to flash: {what}\n  port: {port}\n  chip: {board['chip']}  flash: {board['flash_size']} {board['flash_mode']}")
        if not args.yes and sys.stdin.isatty():
            if input("Continue? [y/N] ").strip().lower() not in ("y", "yes"):
                sys.exit("aborted")

        flash(board, tmp, port, args.mode, args.baud)
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

    p = sub.add_parser("install", help="download and flash")
    p.add_argument("--board", help="board env name (see `list`)")
    p.add_argument("--port", help="serial port (auto-detected if omitted)")
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
