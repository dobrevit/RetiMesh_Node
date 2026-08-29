# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Turn the board's local-link capabilities in boards.json into build flags.

What a PCB puts on its USB connector is a fact about the board, and boards.json
is where the board's facts live: CI, the release packager, the web flasher and
the CLI all read it. The firmware needs the same facts — whether the MCU's own
USB reaches the connector, which bridge sits in front of the UART otherwise,
whether that bridge's DTR/RTS lines reset the chip — and rather than keep a
second copy in a board header that can drift, this runs before every build and
hands the env's "local_link" block to the compiler as -D flags:

    BOARD_USB_NATIVE, BOARD_USB_NCM, BOARD_USB_BRIDGE, BOARD_BRIDGE_AUTO_RESET,
    BOARD_UART_NETWORK, BOARD_UART_MAX_BAUD

The framework's own USB flags come from the same block. ARDUINO_USB_CDC_ON_BOOT
is set exactly when the registry says the chip's USB is on the connector, and
ARDUINO_USB_MODE on every chip that has a USB peripheral at all. An earlier
version left those in platformio.ini and cross-checked them against the
registry — from here with the resolved flags, and from tools/check_boards.py
with a section-name heuristic that could not resolve them — which was the same
rule written twice with two different tests. One source, no check.

An env that `extends` a board env (the local dev build) shares its board's
facts; that is read from the project config rather than from a table here.
Envs with no entry in boards.json (the host-native test env) get nothing, and
Config.h's defaults apply.
"""

import json
from pathlib import Path
from typing import Optional

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
BOARDS = json.loads((ROOT / "boards.json").read_text())


def board_for(env_name: str) -> Optional[dict]:
    """The boards.json entry for an env, following `extends` until one is found."""
    config = env.GetProjectConfig()  # noqa: F821
    seen = set()
    name = env_name
    while name and name not in seen:
        seen.add(name)
        if name in BOARDS:
            return BOARDS[name]
        section = f"env:{name}"
        if not config.has_section(section) or not config.has_option(section, "extends"):
            return None
        # A multi-value option: the config hands it back as a list.
        ext = config.get(section, "extends")
        name = str(ext[0] if isinstance(ext, (list, tuple)) else ext).strip()
        name = name[4:] if name.startswith("env:") else name
    return None


def flags(entry: dict) -> list:
    link = entry.get("local_link", {})
    usb = link.get("usb", {})
    uart = link.get("uart", {})
    native = bool(usb.get("native"))
    out = [
        ("BOARD_USB_NATIVE", 1 if native else 0),
        ("BOARD_USB_NCM", 1 if usb.get("ncm") else 0),
        ("BOARD_USB_BRIDGE", env.StringifyMacro(usb.get("bridge", "none"))),  # noqa: F821
        ("BOARD_BRIDGE_AUTO_RESET", 1 if usb.get("auto_reset_dtr_rts") else 0),
        ("BOARD_UART_NETWORK", 1 if uart.get("network") else 0),
        ("BOARD_UART_MAX_BAUD", int(uart.get("tested_max_baud", 115200))),
    ]
    # The framework's view of the same facts. ARDUINO_USB_MODE says the chip
    # has a USB peripheral, which is a fact about the silicon and so follows
    # the chip; the registry's serial_jtag/otg say whether that peripheral
    # reaches the connector, which is a different fact (the Heltec V3 is an
    # S3 with neither on its socket). CDC_ON_BOOT follows the connector.
    if entry.get("chip") != "esp32":
        out.append(("ARDUINO_USB_MODE", 1))
        if native:
            out.append(("ARDUINO_USB_CDC_ON_BOOT", 1))
    return out


env_name = env.subst("$PIOENV")  # noqa: F821
entry = board_for(env_name)
if entry is None or "local_link" not in entry:
    if env_name != "native":
        print(f"board_caps: no local_link block for [{env_name}] in boards.json — using Config.h defaults")
else:
    defines = flags(entry)
    env.Append(CPPDEFINES=defines)  # noqa: F821
    summary = " ".join(f"{k}={v}" for k, v in defines if k != "BOARD_USB_BRIDGE")
    print(f"board_caps: [{env_name}] {summary} bridge={entry['local_link'].get('usb', {}).get('bridge', 'none')}")
