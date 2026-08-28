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
    section = f"env:{env}"
    if section not in ini:
        problems.append(f"{env}: no [env:{env}] in platformio.ini")
    else:
        flags = ini[section].get("build_flags", "")
        # The same rule board_caps.py enforces at build time, checked here
        # without a toolchain: native USB <=> the framework's CDC-on-boot flag
        # (directly or through the shared [esp32s3_psram_usb] section).
        cdc = "ARDUINO_USB_CDC_ON_BOOT=1" in flags or "esp32s3_psram_usb" in flags
        if bool(usb.get("native")) != cdc:
            problems.append(f"{env}: usb.native={usb.get('native')} but ARDUINO_USB_CDC_ON_BOOT "
                            f"{'is' if cdc else 'is not'} set in platformio.ini")

for p in problems:
    print("boards.json:", p)
print(f"{len([k for k in boards if not k.startswith('_')])} boards checked, {len(problems)} problem(s)")
sys.exit(1 if problems else 0)
