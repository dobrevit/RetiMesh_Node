# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Hand a running node to esptool around `pio run -t upload`.

Before the upload: find the RetiMesh node on the chosen port (or on any port
that looks like one), ask it for its bootloader through the maintenance
console — `BOOTLOADER CONFIRM` on the serial port, which is the fastest path
and needs no credentials — or, if that port is not answering and a node URL is
known, through `POST /api/system/bootloader`. Then wait for the ROM downloader
to appear on the port and let PlatformIO's own esptool invocation carry on
with it. After the upload: wait for the application to come back and print
its VERSION, so a flash that silently left the board in the downloader is
visible right here.

Every step is bounded. When none of it works the hook says what it tried and
steps aside: esptool's own DTR/RTS reset still happens, and a board that needs
a finger on BOOT is told so. Set RETIMESH_NO_AUTO_BOOTLOADER=1 to skip the
hand-off entirely.

The mechanics live in tools/retimesh-flash (retimesh_flash.device), which the
CLI and the HIL scripts share; this file only adapts them to PlatformIO.
"""

import os
import sys
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
sys.path.insert(0, str(ROOT / "tools" / "retimesh-flash"))

try:
    from retimesh_flash import device  # noqa: E402
except Exception as exc:  # pragma: no cover - only when pyserial is missing
    device = None
    _import_error = exc


def _log(msg: str) -> None:
    print(f"retimesh: {msg}")


def before_upload(source, target, env):  # noqa: ARG001
    if os.environ.get("RETIMESH_NO_AUTO_BOOTLOADER") == "1":
        _log("automatic bootloader hand-off disabled by RETIMESH_NO_AUTO_BOOTLOADER")
        return
    if device is None:
        _log(f"bootloader hand-off unavailable ({_import_error}); esptool will try its own reset")
        return

    port = env.subst("$UPLOAD_PORT") or None
    node_url = os.environ.get("RETIMESH_NODE_URL")
    # PlatformIO's interpreter has no esptool package; its bundled esptool.py
    # is what the upload itself will run, so the downloader check runs it too.
    esptool_cmd = [env.subst("$PYTHONEXE"), env.subst("$UPLOADER")]
    try:
        result = device.hand_off_to_bootloader(port=port, node_url_text=node_url, log=_log,
                                               port_hint="--upload-port", esptool_cmd=esptool_cmd)
    except Exception as exc:
        # Stepping aside is the promise: a missing pyserial (imported lazily
        # inside the library, so the import above cannot catch it) or any
        # other failure of the hand-off must not fail the upload it was only
        # meant to ease. esptool's own reset still happens.
        _log(f"bootloader hand-off failed ({exc}); esptool will try its own reset")
        return
    if result.port and (not port or not device.same_device(result.port, port)):
        # Discovery found the node, or the downloader came back under a new
        # name (ttyACM0 -> ttyACM1 on a busy host): point esptool at it.
        _log(f"using {result.port}" + (f" instead of {port}" if port else ""))
        env.Replace(UPLOAD_PORT=result.port)
    # The chip's identity, for after_upload: the downloader's port name
    # carries none, and on a native-USB board the application comes back as
    # another device altogether.
    env.Replace(RETIMESH_NODE_ID=result.node_id)
    if result.entered:
        # entered is only ever true once the node has been seen to go down
        # and come back as the downloader; what esptool does at connect is
        # the hand-off's answer, decided in one place (HandOff.esptool_before).
        _log(f"downloader ready on {result.port} via {result.method}")
        flags = list(env.get("UPLOADERFLAGS", []))
        if "--before" in flags:
            i = flags.index("--before")
            flags[i + 1] = result.esptool_before
            env.Replace(UPLOADERFLAGS=flags)
    else:
        _log(result.message)


def after_upload(source, target, env):  # noqa: ARG001
    if device is None or os.environ.get("RETIMESH_NO_AUTO_BOOTLOADER") == "1":
        return
    port = env.subst("$UPLOAD_PORT") or None
    # The same patience as the hand-off: on a native-USB board the way back
    # from the ROM is a new device on the bus, and a hub that was slow to
    # report the chip leaving is as slow to report it returning.
    wait = 90.0
    info = device.wait_for_application(port=port, timeout=wait, log=_log, node_id=env.get("RETIMESH_NODE_ID"))
    if info:
        _log(f"application is back: {info}")
    else:
        _log(f"the application did not announce itself within {wait:.0f} s; if the board is still in the "
             "downloader, press RST — and if it stays silent, see docs/local-link.md#recovery")


env.AddPreAction("upload", before_upload)   # noqa: F821
env.AddPostAction("upload", after_upload)   # noqa: F821
