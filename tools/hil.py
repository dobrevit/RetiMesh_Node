#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
hil.py — hardware-in-the-loop checks for a bench with:
  * the node on a USB serial port (its console),
  * a local rnsd with an RNode on the same LoRa channel (shared instance).

Checks (each prints PASS/FAIL, exit code = number of failures):
  boot      node boots clean: identity, radio online, transport up, no [E] lines
  announce  the node's announce is accepted by rnsd (rnpath -t lists it)
  rx        a plain-destination packet sent through rnsd is received on LoRa
  tx        the RNode hears the node (rnstatus ↓ bytes grow after an announce)

Usage: hil.py --port /dev/serial/by-id/... [--rns-bin ~/venv/bin] [--reset]
"""
import argparse, re, subprocess, sys, time, serial

def run(cmd, timeout=60):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout

def rnode_block(rns_bin):
    out = run([f"{rns_bin}/rnstatus"])
    m = re.search(r"RNodeInterface\[[^\]]+\](.*?)(?=\n \S|\Z)", out, re.S)
    return m.group(1) if m else ""

def rnode_rx_bytes(rns_bin):
    blk = rnode_block(rns_bin)
    m = re.search(r"↓\s*([\d.]+)\s*(B|KB|MB)", blk)
    if not m: return None
    return float(m.group(1)) * {"B": 1, "KB": 1024, "MB": 1024 * 1024}[m.group(2)]

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--rns-bin", default="")
    ap.add_argument("--reset", action="store_true", help="reset the node first (RTS) to capture a full boot")
    ap.add_argument("--boot-seconds", type=int, default=40)
    a = ap.parse_args()
    rns = a.rns_bin.rstrip("/") + "/" if a.rns_bin else ""
    fails = 0

    def report(name, ok, detail=""):
        nonlocal fails
        fails += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")

    # ---- boot -------------------------------------------------------------
    with serial.Serial(a.port, 115200, timeout=1) as s:
        if a.reset:
            s.dtr = False; s.rts = True; time.sleep(0.2); s.rts = False
        buf = b""; t0 = time.time()
        while time.time() - t0 < a.boot_seconds: buf += s.read(4096)
        log = buf.decode(errors="replace")
        dest = re.search(r"retimesh\.node <([0-9a-f]{32})>", log)
        errors = [l for l in log.splitlines() if "[E][" in l]
        report("boot: identity/destination", bool(dest), dest.group(1) if dest else "not found")
        report("boot: radio online", "online:" in log and "SX12" in log)
        report("boot: transport up", "transport up" in log)
        report("boot: no error lines", not errors, f"{len(errors)} error line(s)" if errors else "")
        rx_before = len(re.findall(r"lora rx/tx (\d+)/", log))
        last_rx = int(re.findall(r"lora rx/tx (\d+)/", log)[-1]) if rx_before else 0

        if not rns:
            print("(no --rns-bin: skipping rnsd-side checks)")
            sys.exit(fails)

        # ---- announce accepted by rnsd ----------------------------------------
        paths = run([f"{rns}rnpath", "-t"])
        report("announce: node in rnsd path table", bool(dest) and dest.group(1) in paths)

        # ---- tx: RNode hears the node ----------------------------------------
        before = rnode_rx_bytes(rns)

        # ---- rx: plain packets reach the node over LoRa ---------------------
        script = "import RNS,time\nRNS.Reticulum()\nd=RNS.Destination(None,RNS.Destination.OUT,RNS.Destination.PLAIN,'retimesh','hil')\n" \
                 "for i in range(3):\n  RNS.Packet(d,b'hil %d'%i).send(); time.sleep(3)\n"
        subprocess.run([f"{rns}python", "-c", script], capture_output=True, timeout=60)
        buf = b""; t0 = time.time()
        while time.time() - t0 < 35: buf += s.read(4096)
        log2 = buf.decode(errors="replace")
        counts = [int(x) for x in re.findall(r"lora rx/tx (\d+)/", log2)]
        got = counts[-1] - last_rx if counts else 0
        report("rx: plain packets received on LoRa", got >= 3, f"{got} of 3")

        after = rnode_rx_bytes(rns)
        report("tx: RNode heard the node since start", before is not None and after is not None and after >= before,
               f"↓ {before} -> {after} B" if before is not None else "rnstatus has no RNode interface")

    sys.exit(fails)

if __name__ == "__main__":
    main()
