#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Check that every board in boards.json describes its host connectivity.

The firmware reads the "local_link" block through tools/board_caps.py; a board
without one silently builds as "no USB, no bridge, nothing", which is a lie for
every board in the registry. CI runs this so the omission is a failed check and
not a node that reports it cannot be flashed.
"""
import configparser
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
boards = json.loads((ROOT / "boards.json").read_text())
ini = configparser.ConfigParser(interpolation=None)
ini.read(ROOT / "platformio.ini")

problems = []
for env, meta in boards.items():
    if env.startswith("_"):
        continue
    link = meta.get("local_link")
    if not link:
        problems.append(f"{env}: no local_link block"); continue
    usb, uart = link.get("usb", {}), link.get("uart", {})
    for key in ("native", "bridge", "auto_reset_dtr_rts"):
        if key not in usb:
            problems.append(f"{env}: local_link.usb.{key} missing")
    if usb.get("native") and usb.get("bridge", "none") != "none":
        problems.append(f"{env}: native USB and a bridge cannot both be on the connector")
    if not usb.get("native") and usb.get("bridge", "none") == "none":
        problems.append(f"{env}: no native USB and no bridge — how is it flashed?")
    if not usb.get("native") and "network" not in uart:
        problems.append(f"{env}: a bridged board should say whether its UART may carry PPP")
    if uart.get("network") and "tested_max_baud" not in uart:
        problems.append(f"{env}: uart.tested_max_baud missing")
    # The framework's USB flags are derived from this block by board_caps.py,
    # so there is nothing in platformio.ini to cross-check them against; only
    # that the env exists at all.
    if f"env:{env}" not in ini:
        problems.append(f"{env}: no [env:{env}] in platformio.ini")

for p in problems:
    print("boards.json:", p)
print(f"{len([k for k in boards if not k.startswith('_')])} boards checked, {len(problems)} problem(s)")
sys.exit(1 if problems else 0)
