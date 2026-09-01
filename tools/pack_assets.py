# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Compress the web assets on their way into the filesystem image.

The A/B partition table leaves the filesystem 128 KiB, and the portal's three
pages are 70 KiB of it. That looked like it fitted, and it does — right up
until the node has been running a while. The Reticulum store lives on the same
filesystem, and once its path table and inbox have grown into what is left, a
node that has been up an hour has four kilobytes free and cannot remember
another identity. It does not fail loudly: it logs a store error per announce
and quietly stops learning paths.

HTML compresses about four to one, so the same three pages take 24 KiB
gzipped and give the store back 46 KiB. serveStatic already looks for
`<path>.gz` and sets Content-Encoding when it finds one, so nothing in the
firmware changes and no page is lost.

Sources stay uncompressed in data/, where they can be read and diffed. This
builds the image from a staging copy instead, so the two never disagree: there
is no committed .gz to fall out of step with the .html beside it.

assets.json is deliberately left alone. The firmware opens it directly rather
than serving it, and LittleFS hands back exactly what is stored.
"""

import gzip
import shutil
from pathlib import Path

Import("env")  # noqa: F821  (injected by PlatformIO)

# Left as-is: read by the firmware, not served through serveStatic.
VERBATIM = {"assets.json"}
# Worth compressing. Anything already compressed (png, woff2) would only grow.
COMPRESS = {".html", ".css", ".js", ".svg", ".json", ".txt"}


def pack(source: Path, staging: Path) -> tuple[int, int]:
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    before = after = 0
    for path in sorted(p for p in source.rglob("*") if p.is_file()):
        raw = path.read_bytes()
        before += len(raw)
        out = staging / path.relative_to(source)
        out.parent.mkdir(parents=True, exist_ok=True)
        if path.name in VERBATIM or path.suffix.lower() not in COMPRESS:
            out.write_bytes(raw)
            after += len(raw)
            continue
        # mtime=0: the image should be identical for identical sources, so a
        # rebuild does not repack the filesystem for no reason.
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        # A file that grew is served as it is; nothing is gained by a .gz that
        # is bigger than what it replaces.
        if len(packed) < len(raw):
            (staging / (str(path.relative_to(source)) + ".gz")).write_bytes(packed)
            after += len(packed)
        else:
            out.write_bytes(raw)
            after += len(raw)
    return before, after


source = Path(env.subst("$PROJECT_DATA_DIR"))  # noqa: F821
if source.is_dir():
    staging = Path(env.subst("$BUILD_DIR")) / "assets"  # noqa: F821
    before, after = pack(source, staging)
    env.Replace(PROJECT_DATA_DIR=str(staging))  # noqa: F821
    saved = before - after
    print(f"web assets: {before} -> {after} bytes "
          f"({saved} freed for the Reticulum store)")
