#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
hil_bootloader.py — hardware-in-the-loop checks for the maintenance console,
the bootloader manager and the flashing hand-off, on a bench with one node on
USB. Runs on the self-hosted HIL runner (.github/workflows/hil.yml), never in
ordinary CI: every step here needs a real port.

Checks (PASS/FAIL each, exit code = failures):
  console    VERSION answers on the port and names the board
  links      NETWORK_STATUS lists the Wi-Fi link(s) with a phase
  api        GET /api/status over --ip (if given) reports the same firmware
  bootloader BOOTLOADER CONFIRM is accepted (or 501 on a classic ESP32)
  rom        the port is back and esptool can read the chip id (download mode)
  flash      esptool writes --firmware at 0x10000 (skipped without --firmware)
  return     the application comes back and VERSION answers again

Usage:
  hil_bootloader.py --port /dev/serial/by-id/... [--ip http://10.42.0.1]
                    [--firmware .pio/build/t3s3/firmware.bin] [--chip esp32s3]
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "retimesh-flash"))
from hilreport import Reporter  # noqa: E402
from retimesh_flash import device as dev  # noqa: E402
# One argument builder, so the esptool 4/5 spelling rule is applied in one place.
from retimesh_flash.device import esptool_args  # noqa: E402


def esptool(chip, port, before, after, *cmd, timeout=60):
    return subprocess.run([sys.executable, "-m", "esptool", *esptool_args(chip, port, before, after, *cmd)],
                          capture_output=True, text=True, timeout=timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--ip", help="node URL over Wi-Fi/USB for the HTTP checks")
    ap.add_argument("--firmware", help="firmware.bin to flash once in the downloader")
    ap.add_argument("--chip", default="esp32s3")
    a = ap.parse_args()
    report = Reporter()

    # One session for every question: reopening the port re-raises the modem
    # lines, which is the one thing Console.open() takes care not to do.
    try:
        con = dev.Console.open(a.port, timeout=3.0)
    except Exception as exc:
        report("console: VERSION answers", False, f"could not open {a.port}: {exc}")
        sys.exit(report.fails)
    info = dev.probe_console(a.port, timeout=3.0, console=con)
    report("console: VERSION answers", info is not None, str(info) if info else "no RM reply")
    if not info:
        con.close()
        sys.exit(report.fails)

    status, _, data = con.command("NETWORK_STATUS")
    names = [d.get("link") for d in data]
    report("links: NETWORK_STATUS lists wifi-ap", status == "OK" and "wifi-ap" in names, ", ".join(
        f"{d.get('link')}={d.get('phase')}" for d in data))
    status, _, data = con.command("USB_STATUS")
    # software_entry only ever arrives on a data line; the OK line carries no
    # key-value pairs for this command.
    software = any(d.get("software_entry") == "yes" for d in data)
    report("usb: USB_STATUS answers", status == "OK", " ".join(f"{k}={v}" for d in data for k, v in d.items()))
    con.close()

    if a.ip:
        hinfo = dev.probe_http(a.ip)
        report("api: /api/status over the link", hinfo is not None and hinfo.version == info.version,
               str(hinfo) if hinfo else "no answer")

    r = dev.hand_off_to_bootloader(port=a.port, log=lambda m: print("       " + m))
    if software:
        report("bootloader: software entry accepted", r.entered and r.method == "console", r.message)
    else:
        report("bootloader: software entry refused politely, esptool's reset offered", r.method == "auto_reset_dtr_rts", r.message)

    port = r.port or a.port
    # One esptool launch here, with the reset the application needs folded
    # in when no firmware follows; the downloader was already confirmed by
    # the hand-off's own sync where entered is true.
    rc = esptool(a.chip, port, r.esptool_before, "hard_reset" if not a.firmware else "no_reset", "chip_id")
    tail = (rc.stdout + rc.stderr).strip().splitlines()
    report("rom: esptool reaches the downloader", rc.returncode == 0, tail[-1] if tail else "")

    if a.firmware:
        rc = esptool(a.chip, port, "no_reset", "hard_reset", "write_flash", "0x10000", a.firmware, timeout=300)
        report("flash: esptool wrote the application", rc.returncode == 0)


    # No pause first: wait_for_application asks VERSION rather than waiting
    # for a banner, so it is bounded by the node answering, not by a guess.
    back = dev.wait_for_application(port, timeout=40.0)
    report("return: application is back", back is not None, str(back) if back else "no VERSION within 40 s")
    sys.exit(report.fails)


if __name__ == "__main__":
    main()
