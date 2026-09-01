# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
"""Fail a build whose image cannot fit the slot it will be installed into.

A node that cannot be reached by cable installs its own updates, and it does
that by writing a whole firmware into the app partition it is not running from.
An image larger than that partition is not a tight fit, it is an update that
cannot be applied — discovered at the worst possible moment, on hardware nobody
can reach, by a node that has just downloaded 1.8 MB to no purpose.

PlatformIO's own `board_upload.maximum_size` does not stop this: it feeds the
"Flash: NN%" line and nothing else, and a build 8 % over its stated maximum
still succeeds. Verified, not assumed — that is why this script exists.

The size comes from the partition table rather than from a number written here,
so the two cannot disagree: the table is the thing the bootloader obeys.
"""

import csv
import os
import re
import sys
from pathlib import Path

Import("env")  # noqa: F821


def slot_bytes() -> "tuple[int, str] | tuple[None, None]":
    """The size of the app partition an OTA image is written into, and the name
    of the table it came from. None where the env has no CSV table (a board on
    the framework's stock layout, or the host-native env)."""
    table = env.subst("$PARTITIONS_TABLE_CSV")  # noqa: F821
    if not table or not Path(table).is_file():
        return None, None
    smallest = None
    with open(table, newline="") as f:
        for row in csv.reader(f):
            cells = [c.strip() for c in row if c.strip()]
            if len(cells) < 5 or cells[0].startswith("#"):
                continue
            name, kind, size = cells[0], cells[1], cells[4]
            if kind != "app":
                continue
            try:
                value = int(size, 0) if not size.lower().endswith(("k", "m")) else \
                        int(size[:-1], 0) * (1024 if size.lower().endswith("k") else 1024 * 1024)
            except ValueError:
                continue
            # The smallest app partition, because the image has to fit whichever
            # slot it is installed into, not the roomiest one.
            if smallest is None or value < smallest:
                smallest = value
    return smallest, Path(table).name


def check(source, target, env):  # noqa: ARG001, F811
    image = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    slot, table = slot_bytes()
    if slot is None or not image.is_file():
        return
    size = image.stat().st_size
    headroom = slot - size
    if headroom < 0:
        print(f"\n*** image is {-headroom} bytes larger than the {slot // 1024} KiB app "
              f"partition in {table}.")
        print("*** It would build, flash over a cable, and then be unable to install its")
        print("*** own updates. Trim the build or change the table — but a table can only")
        print("*** be changed by writing the whole flash over a cable, which is not")
        print("*** something a deployed node can be offered.")
        env.Exit(1)
    pct = 100.0 * size / slot
    print(f"image: {size} bytes, {pct:.1f}% of the {slot // 1024} KiB app slot "
          f"({headroom // 1024} KiB free for growth)")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check)  # noqa: F821
