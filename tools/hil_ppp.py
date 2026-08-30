#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
hil_ppp.py — hardware-in-the-loop checks for PPP over the bridge UART, on a
bench with one CP2102/CH9102 node on USB and pppd installed. Runs on the
self-hosted HIL runner (.github/workflows/hil.yml), never in ordinary CI:
every step needs a real port, and pppd needs root — this script is run
under sudo, or with a sudoers rule for pppd and kill, and says so when it
cannot start pppd.

Checks (PASS/FAIL each, exit code = failures):
  console    VERSION answers on the port and LINKS names the PPP link
  enable     PPP ON is accepted (the switch applies live, no restart)
  ppp_up     pppd brings ppp0 up with the addresses the node asked for
  api        GET /api/status over ppp0 reports the same firmware
  bootloader POST /api/system/bootloader over ppp0 is accepted (or 501 on a classic ESP32)
  ppp_down   pppd exits and releases the port (the node closes LCP as it restarts)
  rom        esptool reaches the downloader on the port
  flash      esptool writes --firmware at 0x10000 (skipped without --firmware)
  return     the application comes back and VERSION answers again
  ppp_again  pppd brings ppp0 up again and /api/status answers over it
  restore    PPP is switched back to what it was

Usage:
  sudo hil_ppp.py --port /dev/serial/by-path/... [--firmware .pio/build/heltec-v3/firmware.bin]
                  [--chip esp32s3] [--password retimesh]
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "retimesh-flash"))
from hilreport import Reporter  # noqa: E402
from retimesh_flash import device as dev  # noqa: E402
from retimesh_flash.device import esptool_args  # noqa: E402


def esptool(chip, port, before, after, *cmd, timeout=60):
    return subprocess.run([sys.executable, "-m", "esptool", *esptool_args(chip, port, before, after, *cmd)],
                          capture_output=True, text=True, timeout=timeout)


def start_pppd(port, node_ip, host_ip, baud):
    """pppd in the foreground (nodetach), as a child; None when it cannot be started."""
    cmd = dev.pppd_command(port, node_ip, host_ip, baud)
    try:
        return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as exc:
        print(f"       cannot start pppd: {exc}")
        return None


def wait_link(timeout):
    """The first PPP link the routing table shows within `timeout` seconds."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        links = dev.ppp_links()
        if links:
            return links[0]
        time.sleep(0.5)
    return None


def wait_exit(proc, timeout):
    try:
        proc.wait(timeout=timeout)
        return True
    except subprocess.TimeoutExpired:
        return False


def stop_pppd(proc):
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        wait_exit(proc, 10)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--firmware", help="firmware.bin to flash once in the downloader")
    ap.add_argument("--chip", default="esp32s3")
    ap.add_argument("--password", default=dev.DEFAULT_ADMIN[1], help="admin password for the HTTP path")
    a = ap.parse_args()
    report = Reporter()
    if os.geteuid() != 0:
        print("pppd needs root: run this script with sudo (or give the runner a sudoers rule for pppd)")
    if dev.find_pppd(a.port):
        sys.exit(f"a pppd already holds {a.port}; stop it first")

    # --- console, links ------------------------------------------------------
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
    status, _, data = con.command("LINKS")
    addresses = dev.ppp_addresses(data)
    report("console: LINKS names the PPP link", status == "OK" and addresses is not None,
           " ".join(f"{d.get('link')}={d.get('enabled')}" for d in data))
    if addresses is None:
        con.close()
        sys.exit(report.fails)
    node_ip, host_ip, baud, was_enabled = addresses
    status, kv, _ = con.command("PPP ON")
    report("enable: PPP ON accepted", status == "OK", f"{status} {kv}")
    con.close()

    # --- ppp up, api ---------------------------------------------------------
    proc = start_pppd(a.port, node_ip, host_ip, baud)
    link = wait_link(30.0) if proc else None
    report("ppp_up: pppd brought ppp0 up with the node's addresses",
           link is not None and link.node_ip == node_ip, f"{link}" if link else "no ppp route within 30 s")
    if link is None:
        stop_pppd(proc)
        sys.exit(report.fails)
    hinfo = dev.probe_http(link.url, timeout=5.0)
    report("api: /api/status over ppp0", hinfo is not None and hinfo.version == info.version,
           str(hinfo) if hinfo else "no answer")

    # --- bootloader over ppp0, ppp down, rom, flash, return ------------------
    ok, msg, _ = dev.request_bootloader_http(link.url, (dev.DEFAULT_ADMIN[0], a.password))
    software = ok
    if ok:
        report("bootloader: request over ppp0 accepted", True, msg)
    else:
        report("bootloader: refused politely on a classic ESP32", "501" in msg, msg)
    if software:
        gone = wait_exit(proc, 45.0)
        report("ppp_down: pppd exited as the node went down", gone, "" if gone else "pppd still running after 45 s")
        if not gone:
            stop_pppd(proc)
    else:
        # esptool has to do the reset, and needs the port: pppd is stopped here.
        stop_pppd(proc)
        report("ppp_down: pppd stopped for esptool", proc.poll() is not None)
    rc = esptool(a.chip, a.port, "default_reset", "hard_reset" if not a.firmware else "no_reset", "chip_id")
    tail = (rc.stdout + rc.stderr).strip().splitlines()
    report("rom: esptool reaches the downloader", rc.returncode == 0, tail[-1] if tail else "")
    if a.firmware:
        rc = esptool(a.chip, a.port, "no_reset", "hard_reset", "write_flash", "0x10000", a.firmware, timeout=300)
        report("flash: esptool wrote the application", rc.returncode == 0)
    wait = dev.application_wait_s(port=a.port)
    back = dev.wait_for_application(a.port, timeout=wait)
    report("return: application is back", back is not None, str(back) if back else f"no VERSION within {wait:.0f} s")

    # --- ppp again, restore --------------------------------------------------
    proc = start_pppd(a.port, node_ip, host_ip, baud)
    link = wait_link(30.0) if proc else None
    hinfo = link and dev.probe_http(link.url, timeout=5.0)
    report("ppp_again: ppp0 up again and /api/status answers", bool(hinfo), str(hinfo) if hinfo else "no answer")
    stop_pppd(proc)
    if not was_enabled:
        con = dev.Console.open(a.port, timeout=3.0)
        status, kv, _ = con.command("PPP OFF")
        con.close()
        report("restore: PPP switched back off", status == "OK", f"{status} {kv}")
    sys.exit(report.fails)


if __name__ == "__main__":
    main()
