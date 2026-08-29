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
    for key in ("native", "bridge", "auto_reset_dtr_rts", "serial_jtag", "otg"):
        if key not in usb:
            problems.append(f"{env}: local_link.usb.{key} missing")
    # serial_jtag/otg say whether the chip's USB unit reaches the connector.
    # A classic ESP32 has none to route, and a board cannot be native-USB
    # without one of them on the socket.
    on_connector = bool(usb.get("serial_jtag") or usb.get("otg"))
    if meta.get("chip") == "esp32" and on_connector:
        problems.append(f"{env}: a classic ESP32 has no USB unit to put on the connector")
    if usb.get("native") and not on_connector:
        problems.append(f"{env}: usb.native without serial_jtag or otg on the connector")
    # NCM is presented by the OTG stack, and a device the host is to
    # recognise needs an identity to present.
    if usb.get("ncm") and not usb.get("otg"):
        problems.append(f"{env}: usb.ncm without usb.otg — the OTG stack is what presents it")
    if usb.get("ncm") and not isinstance(boards.get("_usb_identity"), dict):
        problems.append(f"{env}: usb.ncm but boards.json has no _usb_identity block")
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

ident = boards.get("_usb_identity")
if isinstance(ident, dict):
    for key in ("vid", "pid", "manufacturer", "product", "network_interface"):
        if not ident.get(key):
            problems.append(f"_usb_identity.{key} missing")
    if ident.get("pid_is_test_allocation") is None:
        problems.append("_usb_identity.pid_is_test_allocation missing — say whether the PID may ship")
    # A release may not ship pid.codes' test allocation: the rule the registry
    # states gets its teeth here, from the release workflow (--release).
    if "--release" in sys.argv[1:] and ident.get("pid_is_test_allocation"):
        problems.append("_usb_identity: the PID is a test allocation and may not ship in a release")

for p in problems:
    print("boards.json:", p)
print(f"{len([k for k in boards if not k.startswith('_')])} boards checked, {len(problems)} problem(s)")
sys.exit(1 if problems else 0)
