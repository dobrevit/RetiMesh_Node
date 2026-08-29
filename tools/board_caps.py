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

The framework's own USB flags come from the same block. A board whose own USB
unit is on the connector with OTG and NCM behind it runs the OTG stack
(ARDUINO_USB_MODE 0): TinyUSB owns the peripheral and presents the composite
device — CDC-ACM for the console, CDC-NCM for usb0 — whose identity comes from
the registry's _usb_identity block as the USB_VID/USB_PID/USB_MANUFACTURER/
USB_PRODUCT macros the core's USB.cpp reads. Every other chip with a USB
peripheral keeps the fixed USB-Serial/JTAG unit (mode 1), and
ARDUINO_USB_CDC_ON_BOOT is set exactly when that unit — or the composite
device's ACM port — is what the host sees. An earlier version left those
flags in platformio.ini and cross-checked them against the registry, which was
the same rule written twice with two different tests. One source, no check.

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


def ncm_driver(entry: dict) -> bool:
    """Whether this build drives USB networking: the chip's own USB on the
    connector, with OTG to run the stack and NCM to present."""
    usb = entry.get("local_link", {}).get("usb", {})
    return bool(usb.get("native") and usb.get("otg") and usb.get("ncm"))


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
    # The framework's view of the same facts. ARDUINO_USB_MODE says which
    # stack owns the chip's USB peripheral: 1 is the fixed USB-Serial/JTAG
    # unit, 0 the OTG stack with TinyUSB and the composite device. The
    # registry's serial_jtag/otg say whether the peripheral reaches the
    # connector at all (the Heltec V3 is an S3 with neither on its socket).
    # CDC_ON_BOOT follows the connector: the JTAG unit or the ACM port.
    if entry.get("chip") != "esp32":
        out.append(("ARDUINO_USB_MODE", 0 if ncm_driver(entry) else 1))
        if native:
            out.append(("ARDUINO_USB_CDC_ON_BOOT", 1))
    if ncm_driver(entry):
        ident = BOARDS["_usb_identity"]
        # The VID/PID travel under our own names: the core's variant header
        # defines USB_VID/USB_PID unconditionally after any -D, so the core's
        # own device descriptor cannot be steered from here. UsbNcm.cpp
        # supplies the device descriptor instead and reads these.
        out += [
            ("RETIMESH_USB_VID", ident["vid"]),
            ("RETIMESH_USB_PID", ident["pid"]),
            ("USB_MANUFACTURER", env.StringifyMacro(ident["manufacturer"])),          # noqa: F821
            ("USB_PRODUCT", env.StringifyMacro(ident["product"])),                    # noqa: F821
            ("USB_NETWORK_INTERFACE", env.StringifyMacro(ident["network_interface"])),  # noqa: F821
            ("USB_PID_IS_TEST_ALLOCATION", 1 if ident.get("pid_is_test_allocation") else 0),
        ]
    return out


env_name = env.subst("$PIOENV")  # noqa: F821
entry = board_for(env_name)
if entry is None or "local_link" not in entry:
    if env_name != "native":
        print(f"board_caps: no local_link block for [{env_name}] in boards.json — using Config.h defaults")
else:
    defines = flags(entry)
    # The board definition carries its own ARDUINO_USB_MODE; ours replaces it
    # rather than sitting beside it with the other value. Only the other
    # value is unflagged: an unflag matches our own define too, and a build
    # with the macro missing is one the core reads as OTG by accident.
    mode = dict(defines).get("ARDUINO_USB_MODE")
    if mode is not None:
        env.Append(BUILD_UNFLAGS=[f"-DARDUINO_USB_MODE={1 - mode}"])  # noqa: F821
    env.Append(CPPDEFINES=defines)  # noqa: F821
    summary = " ".join(f"{k}={v}" for k, v in defines if k != "BOARD_USB_BRIDGE")
    print(f"board_caps: [{env_name}] {summary} bridge={entry['local_link'].get('usb', {}).get('bridge', 'none')}")
