#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Read the node's telemetry back with Sideband's own Telemeter.

A telemetry value in the wrong msgpack type or the wrong shape is not rendered
wrongly by a client — it is dropped. So the shapes in src/rns/Telemetry.h are
not something to reason about; they are something to check against the class
that will actually read them.

test/test_telemetry/ holds the part CI can run: it takes the document apart
with this node's own decoder and asserts the shapes. This goes further and
asks the real app, which needs Sideband's source, so it is a bench check to be
run when a sensor is added or its shape changes.

    pip install rns lxmf
    pip download sbapp --no-deps --no-binary :all: -d /tmp/sb
    tar -xzf /tmp/sb/sbapp-*.tar.gz -C /tmp/sb
    python tools/telemetry_check.py /tmp/sb/sbapp-1.8.0

Prints what each sensor reads back as. A sensor missing from the output is one
the app silently dropped — which is exactly the failure this is here to catch,
and the reason "it looked right in the hex" is not enough.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# A snapshot with everything, and one with what a bare board can answer. The
# second matters as much as the first: most boards have no receiver and cannot
# see their charger, and the document they send has to be readable too.
EMITTER = r"""
#include "Telemetry.h"
#include <cstdio>
int main(int argc, char** argv) {
  const bool full = argc > 1 && argv[1][0] == 'f';
  Rns::Telemetry::Snapshot s;
  s.utc = 1767225600;
  s.information = "RetiMesh Node v0.1.0 (LilyGO T3-S3)";
  s.haveBattery = true; s.batteryPercent = 87.5f; s.charging = true; s.chargeKnown = full;
  if (full) {
    s.havePosition = true; s.latitude = 42.6977; s.longitude = 23.3219;
    s.altitudeM = 595.0f; s.speedKmh = 0.0f; s.accuracyM = 7.5f;
    s.positionAt = 1767225590;
  }
  s.haveSignal = true; s.rssi = -104.0f; s.snr = 8.75f; s.quality = 62;
  s.haveProcessor = true; s.cpuHz = 240000000;
  s.haveMemory = true; s.heapCapacity = 327680; s.heapUsed = 228164;
  s.haveStorage = true; s.flashCapacity = 3145728; s.flashUsed = 1751662;
  uint8_t buf[512];
  const size_t n = Rns::Telemetry::build(s, buf, sizeof(buf));
  if (!n) { printf("EMPTY\n"); return 1; }
  for (size_t i = 0; i < n; i++) printf("%02x", buf[i]);
  printf("\n");
  return 0;
}
"""

EXPECTED = {"time", "information", "battery", "physical_link", "processor", "ram", "nvm"}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sys.path.insert(0, os.path.abspath(sys.argv[1]))
    from sbapp.sideband.sense import Telemeter

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "emit.cpp")
        exe = os.path.join(tmp, "emit")
        open(src, "w").write(EMITTER)
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra",
                        "-I", os.path.join(ROOT, "src/rns"), "-o", exe, src], check=True)

        failed = False
        for mode, label in (("f", "a board with a fix and a charger it can see"),
                            ("m", "a board with neither")):
            hexed = subprocess.run([exe, mode], capture_output=True, text=True,
                                   check=True).stdout.strip()
            packed = bytes.fromhex(hexed)
            t = Telemeter.from_packed(packed)
            if t is None:
                print(f"{label}: the document was REFUSED outright")
                failed = True
                continue
            readings = t.read_all()
            print(f"=== {label} — {len(packed)} bytes, {len(readings)} sensors ===")
            for name, data in readings.items():
                print(f"  {name:16} {data!r}")
            missing = EXPECTED - set(readings)
            if mode == "f":
                missing -= set()
                if "location" not in readings:
                    missing.add("location")
            if missing:
                print(f"  DROPPED by the app: {sorted(missing)}")
                failed = True
            print()
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
