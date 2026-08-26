#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
bump_deps.py — Dependabot for platformio.ini.

Dependabot has no PlatformIO ecosystem, so this script asks the PlatformIO
registry for the newest version of every `owner/name @ ^x.y.z` lib_deps
entry (and the `espressif32 @ ^x.y.z` platform), rewrites the caret base
in place, and prints a Markdown summary for the PR body. Major bumps are
skipped unless --allow-major is given: a new ESP32 platform major means a
new ESP-IDF, which deserves a human.
"""
import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

INI = Path(__file__).resolve().parent.parent / "platformio.ini"
REGISTRY = "https://api.registry.platformio.org/v3/packages/{owner}/{type}/{name}"
LIB_RE = re.compile(r"^(\s*)([\w-]+)/([\w.-]+)(\s*@\s*\^)(\d+)\.(\d+)\.(\d+)(\s*)$")
PLATFORM_RE = re.compile(r"^(platform\s*=\s*)(espressif32)(\s*@\s*\^)(\d+)\.(\d+)\.(\d+)(\s*)$")


def latest(owner: str, ptype: str, name: str) -> tuple[int, int, int]:
    with urllib.request.urlopen(REGISTRY.format(owner=owner, type=ptype, name=name), timeout=30) as r:
        data = json.load(r)
    return tuple(int(x) for x in data["version"]["name"].split(".")[:3])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--allow-major", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    lines = INI.read_text().splitlines(keepends=True)
    rows, changed = [], False
    for i, line in enumerate(lines):
        m = LIB_RE.match(line.rstrip("\n"))
        p = PLATFORM_RE.match(line.rstrip("\n"))
        if m:
            indent, owner, name, sep, *ver, tail = m.groups()
            label, ptype = f"{owner}/{name}", "library"
        elif p:
            indent, name, sep, *ver, tail = p.groups()
            owner, label, ptype = "platformio", "espressif32 platform", "platform"
        else:
            continue
        cur = tuple(int(v) for v in ver)
        try:
            new = latest(owner, ptype, name)
        except Exception as e:  # registry hiccup: report, keep going
            rows.append((label, ".".join(map(str, cur)), f"lookup failed: {e}", ""))
            continue
        if new <= cur:
            continue
        if new[0] != cur[0] and not args.allow_major:
            rows.append((label, ".".join(map(str, cur)), ".".join(map(str, new)), "major — skipped"))
            continue
        lines[i] = line.replace(".".join(map(str, cur)), ".".join(map(str, new)), 1)
        rows.append((label, ".".join(map(str, cur)), ".".join(map(str, new)), "bumped"))
        changed = True

    if changed and not args.dry_run:
        INI.write_text("".join(lines))

    if not rows:
        print("All PlatformIO dependencies are up to date.")
        return
    print("| Package | Current | Latest | Action |\n|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(r) + " |")
    sys.exit(0)


if __name__ == "__main__":
    main()
