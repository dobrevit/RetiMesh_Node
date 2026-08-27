# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Stamp the web assets so the node can tell when they do not match its firmware.

Firmware and filesystem are flashed separately, and nothing forces them to be
updated together. A firmware-only update leaves a node serving a portal built
against an older API — silently, and looking perfectly healthy. That is not
hypothetical: it is how a settings page came to be missing a control the
firmware underneath it already supported, and the only way to find out was to
diff the served page by hand.

The cure is cheap. This runs before the build, hashes everything under data/,
writes the hash into the image as /assets.json, and hands the same hash to the
firmware as ASSET_STAMP. Two copies of one number, produced together and
compared at runtime: if they differ, the two halves came from different builds
and the node says so.

The point is that this replaces reflashing-to-be-sure. Writing the filesystem
erases whatever else lives on it — on a board without an SD card that includes
the Reticulum transport store, so a needless upload costs the node its path
table. Being told the halves already agree is worth more than the reflash it
avoids.
"""

import hashlib
import json
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

STAMP_FILE = "assets.json"


def compute_stamp(data_dir: Path) -> str:
    """A hash over every asset, name and content, in a stable order.

    The stamp file itself is excluded — it holds the answer, so including it
    would be self-referential and would change the hash on every run.
    """
    digest = hashlib.sha256()
    for path in sorted(p for p in data_dir.rglob("*") if p.is_file()):
        if path.name == STAMP_FILE:
            continue
        digest.update(path.relative_to(data_dir).as_posix().encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


data_dir = Path(env.subst("$PROJECT_DATA_DIR"))  # noqa: F821

if not data_dir.is_dir():
    # Environments with no web assets (the native test build) have nothing to
    # stamp, and must not fail the build for it.
    env.Append(CPPDEFINES=[("ASSET_STAMP", env.StringifyMacro("none"))])  # noqa: F821
else:
    stamp = compute_stamp(data_dir)
    written = json.dumps({"stamp": stamp}, separators=(",", ":")) + "\n"
    target = data_dir / STAMP_FILE
    # Only rewrite when it actually changed, so a build does not needlessly
    # mark the filesystem image dirty and trigger a repack.
    if not target.exists() or target.read_text() != written:
        target.write_text(written)
    env.Append(CPPDEFINES=[("ASSET_STAMP", env.StringifyMacro(stamp))])  # noqa: F821
    print(f"asset stamp: {stamp} ({data_dir})")
