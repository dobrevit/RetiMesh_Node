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

platformio.ini still carries the framework's own USB flags (ARDUINO_USB_MODE,
ARDUINO_USB_CDC_ON_BOOT), because the framework reads those and nothing else.
The two descriptions overlap on one fact — native USB on the connector — and
this script refuses to build when they disagree, which is the failure a board
header copy would have hidden: a board declared native-USB in the registry but
built with its console on a UART nobody can reach, or the reverse.

Envs with no entry in boards.json (the host-native test env) get nothing, and
Config.h's defaults apply.
"""

import json
from pathlib import Path
from typing import Optional

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
BOARDS = json.loads((ROOT / "boards.json").read_text())

# A local dev env that extends a board env shares its hardware. Only the one
# case that exists is mapped, so an unmapped env is a loud omission.
ALIASES = {"t3s3-local": "t3s3"}


def caps_for(env_name: str) -> Optional[dict]:
    entry = BOARDS.get(ALIASES.get(env_name, env_name))
    return entry.get("local_link") if entry else None


def flags(link: dict) -> list:
    usb = link.get("usb", {})
    uart = link.get("uart", {})
    out = [
        ("BOARD_USB_NATIVE", 1 if usb.get("native") else 0),
        ("BOARD_USB_NCM", 1 if usb.get("ncm") else 0),
        ("BOARD_USB_BRIDGE", env.StringifyMacro(usb.get("bridge", "none"))),  # noqa: F821
        ("BOARD_BRIDGE_AUTO_RESET", 1 if usb.get("auto_reset_dtr_rts") else 0),
        ("BOARD_UART_NETWORK", 1 if uart.get("network") else 0),
        ("BOARD_UART_MAX_BAUD", int(uart.get("tested_max_baud", 115200))),
    ]
    return out


def check_against_framework(env_name: str, link: dict) -> None:
    """The one overlapping fact must agree, or the build stops here."""
    build_flags = " ".join(str(f) for f in env.get("BUILD_FLAGS", []))  # noqa: F821
    cdc_on_boot = "ARDUINO_USB_CDC_ON_BOOT=1" in build_flags
    native = bool(link.get("usb", {}).get("native"))
    if native != cdc_on_boot:
        raise SystemExit(
            f"board_caps: [{env_name}] boards.json says usb.native={native} but platformio.ini "
            f"{'sets' if cdc_on_boot else 'does not set'} ARDUINO_USB_CDC_ON_BOOT=1 — "
            "the two must agree, or the console ends up on a port nobody can reach"
        )


env_name = env.subst("$PIOENV")  # noqa: F821
link = caps_for(env_name)
if link is None:
    if env_name != "native":
        print(f"board_caps: no local_link block for [{env_name}] in boards.json — using Config.h defaults")
else:
    check_against_framework(env_name, link)
    defines = flags(link)
    env.Append(CPPDEFINES=defines)  # noqa: F821
    summary = " ".join(f"{k}={v}" for k, v in defines if k != "BOARD_USB_BRIDGE")
    print(f"board_caps: [{env_name}] {summary} bridge={link.get('usb', {}).get('bridge', 'none')}")
