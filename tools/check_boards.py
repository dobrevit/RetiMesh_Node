#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Check that the board registry, the build environments and Config.h agree.

Three checks, each of which exists because its absence is silent.

The "local_link" block: the firmware reads it through tools/board_caps.py, and a
board without one builds as "no USB, no bridge, nothing" — a lie for every board
in the registry, and a node that reports it cannot be flashed.

An env with no entry: CI, the release matrix and the HIL run all take their
board list from boards.json, so an env the registry does not know is built by
nobody. It compiles on the machine of whoever added it and then quietly stops
being compiled at all.

A -DBOARD_* flag Config.h does not dispatch: the board-selection chain ends in
an #else that includes the T3-S3's header, so a flag with no arm — a typo, or a
header added without its line — is not a build error. It is a board that builds
successfully, boots, and drives another board's pin map.
"""
import configparser
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
boards = json.loads((ROOT / "boards.json").read_text())
ini = configparser.ConfigParser(interpolation=None)
ini.read(ROOT / "platformio.ini")

problems = []      # a failed check: the exit code is non-zero
warnings = []      # said out loud, but the check still passes
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
    # The PPP baud rule (LocalLinkState.h, pppBaudAllowed) takes its ladder
    # from `qualification` and its ceiling from `tested_max_baud`; the
    # firmware refuses what is not listed or is above the ceiling, so the
    # data has to be the shape the rule expects: ascending, the console's
    # 115200 among them (the speed everything else on the port runs at), and
    # the ceiling one of the rungs.
    if uart.get("network"):
        rungs = uart.get("qualification")
        if not isinstance(rungs, list) or not rungs or any(not isinstance(b, int) or b <= 0 for b in rungs):
            problems.append(f"{env}: uart.qualification must list the bauds PPP may run at")
        else:
            if rungs != sorted(set(rungs)):
                problems.append(f"{env}: uart.qualification must be ascending with no repeats")
            if 115200 not in rungs:
                problems.append(f"{env}: uart.qualification must include 115200, the console's speed")
            if uart.get("tested_max_baud") not in rungs:
                problems.append(f"{env}: uart.tested_max_baud must be one of uart.qualification")
        if "instance" not in uart:
            problems.append(f"{env}: uart.instance missing — which UART the bridge is on")
    # The framework's USB flags are derived from this block by board_caps.py,
    # so there is nothing in platformio.ini to cross-check them against; only
    # that the env exists at all.
    if f"env:{env}" not in ini:
        problems.append(f"{env}: no [env:{env}] in platformio.ini")

# The other direction: an env that is a board and is not in the registry.
# `native` is the host test env and has no board; an env that `extends` another
# shares its board's entry, which is how board_caps.py resolves it too.
for section in ini.sections():
    if not section.startswith("env:"):
        continue
    env = section[len("env:"):]
    if env == "native" or ini.has_option(section, "extends"):
        continue
    if env not in boards:
        problems.append(f"{env}: [{section}] builds a board that boards.json does not list — "
                        "CI, the release matrix and the HIL run all read the registry, so "
                        "nothing would ever build it")

# The board-selection chain in src/Config.h, read as what it is: a list of
# (flag, header) pairs. Reading the flags and the headers as two separate sets
# was the first version of this check and it missed the mistake most worth
# catching — an arm whose header is the one above it, copied and not changed.
# Both names then exist in the file and every set-membership test passes, and
# the board builds on another board's pin map.
#
# The include has to be on the line after the #elif, which is how the chain is
# written. A comment wedged between them drops that arm and the env's flag is
# then reported as undispatched — wrong reason, right outcome, and loud.
config_h = (ROOT / "src" / "Config.h").read_text()
arms = re.findall(r'defined\((BOARD_[A-Z0-9_]+)\)\s*\)?\s*\n\s*#include\s+"boards/([a-z0-9_]+)\.h"',
                  config_h)
dispatched = {flag: header for flag, header in arms}
for flag, header in arms:
    if not (ROOT / "src" / "boards" / f"{header}.h").exists():
        problems.append(f"src/Config.h dispatches -D{flag} to boards/{header}.h, which does not exist")
# One header per flag and one flag per header. Two arms including the same
# header is the copy-paste this pairing exists to catch.
for header in {h for _, h in arms}:
    flags = sorted(f for f, h in arms if h == header)
    if len(flags) > 1:
        problems.append(f"src/Config.h dispatches {' and '.join(flags)} to the same "
                        f"boards/{header}.h — one of them is a copy that was not changed")

# Envs that deliberately build the chain's closing #else — the T3-S3's header —
# rather than naming a board of their own. Two of them, each for a stated
# reason, and the list is here rather than inferred because "no flag" and
# "forgot the flag" are indistinguishable from the outside:
#   t3s3          the board the #else names; its own env passes no flag
#   esp32s3-qspi  the same header with the LoRa pins overridden per wiring
DEFAULT_HEADER_ENVS = {"t3s3", "esp32s3-qspi"}

seen_flags = {}
for section in ini.sections():
    if not section.startswith("env:"):
        continue
    env = section[len("env:"):]
    if env == "native" or ini.has_option(section, "extends"):
        continue
    flags = [f for f in re.findall(r"-D(BOARD_[A-Z0-9_]+)",
                                   ini.get(section, "build_flags") if ini.has_option(section, "build_flags") else "")
             # BOARD_HAS_PSRAM is the framework's own, not a board selector.
             if f != "BOARD_HAS_PSRAM"]
    for flag in flags:
        if flag not in dispatched:
            problems.append(f"{env}: passes -D{flag}, which src/Config.h never tests — the "
                            "board-selection chain would fall through to the T3-S3's header "
                            "and the build would succeed on the wrong pin map")
        if flag in seen_flags:
            problems.append(f"{env}: passes -D{flag}, which {seen_flags[flag]} already uses — "
                            "two envs on one board header, so one of them builds the other "
                            "board's pin map")
        else:
            seen_flags[flag] = env
    if not flags and env not in DEFAULT_HEADER_ENVS:
        problems.append(f"{env}: passes no -DBOARD_* flag, so src/Config.h's #else builds it "
                        "as a T3-S3 — name the board, or add the env to DEFAULT_HEADER_ENVS "
                        "here if that really is what it wants")

ident = boards.get("_usb_identity")
if isinstance(ident, dict):
    for key in ("vid", "pid", "manufacturer", "product", "network_interface"):
        if not ident.get(key):
            problems.append(f"_usb_identity.{key} missing")
    if ident.get("pid_is_test_allocation") is None:
        problems.append("_usb_identity.pid_is_test_allocation missing — say whether the PID may ship")
    # pid.codes' test allocation is for development and their policy does not
    # permit shipping it, which is a thing to say loudly at every release
    # (--release) and not a thing to stop one over: the tag builds eight
    # boards and six of them never present the composite device at all. The
    # release notes carry the warning; obtaining a PID is the fix.
    if "--release" in sys.argv[1:] and ident.get("pid_is_test_allocation"):
        warnings.append("_usb_identity: the PID is pid.codes' test allocation, which is not for shipping "
                        "— request one per github.com/espressif/usb-pids before this release is published")

for p in problems:
    print("boards.json:", p)
for w in warnings:
    print("boards.json: warning:", w)
print(f"{len([k for k in boards if not k.startswith('_')])} boards checked against "
      f"platformio.ini and src/Config.h, "
      f"{len(problems)} problem(s), {len(warnings)} warning(s)")
sys.exit(1 if problems else 0)
