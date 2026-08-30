# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
"""Stamp a local build with the commit it was built from.

A release gets its version from CI, which passes the tag through
PLATFORMIO_BUILD_FLAGS. Everything else said "dev" — every board on a bench,
all answering VERSION with the same three letters whatever code is on them.

That is enough to lose a week. Six nodes were flashed from two different
commits in one afternoon and every one of them reported version=dev; the
asset stamp did not distinguish them either, because it hashes data/ and
data/ had not changed between those commits. A soak was started against the
fleet before anybody noticed that half of it was running superseded code.

So a build with no version handed to it asks git for one. `git describe
--always --dirty` gives the nearest tag, the distance from it and the commit
— or just the commit in a repository with no tags — and says when the tree
had uncommitted changes, which is the difference between "this is v0.1.4"
and "this is something a person was in the middle of".

Nothing here overrides CI: a version supplied by the build environment wins,
and a checkout with no git at all (a release tarball, a container without the
binary) falls back to Config.h's "dev" exactly as before.

One cost, stated because it is not free: FW_VERSION reaches every translation
unit through Config.h, so when its value changes the whole environment
rebuilds. The value changes when HEAD moves or when the tree first becomes
dirty — not on every build, and not while you keep editing — so it is a
rebuild per commit rather than a rebuild per build. tools/asset_stamp.py has
the same shape and the same cost for the same reason.
"""

import os
import subprocess
from pathlib import Path
from typing import Optional

Import("env")  # noqa: F821  (injected by PlatformIO)

ROOT = Path(env.subst("$PROJECT_DIR"))  # noqa: F821


def version_supplied() -> bool:
    """Whether something already said what this firmware is.

    CI's -DFW_VERSION arrives in PLATFORMIO_BUILD_FLAGS, which PlatformIO
    folds into the environment's own flags — but not necessarily before a
    pre-script runs, so the raw variable is checked as well as the defines
    that have been parsed so far. A false negative here would overwrite a
    release's tag with a commit hash, which is the one outcome worth being
    careful about.
    """
    for define in env.get("CPPDEFINES", []):  # noqa: F821
        name = define[0] if isinstance(define, (list, tuple)) else define
        if str(name) == "FW_VERSION":
            return True
    return "FW_VERSION" in os.environ.get("PLATFORMIO_BUILD_FLAGS", "")


def git_version() -> Optional[str]:
    """What git calls this checkout, or None where git cannot say."""
    try:
        result = subprocess.run(
            ["git", "describe", "--always", "--dirty"],
            cwd=str(ROOT), capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return None                      # no git binary, or it would not run
    if result.returncode != 0:
        return None                      # not a repository, or no commits yet
    return result.stdout.strip() or None


if not version_supplied():
    version = git_version()
    if version:
        env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])  # noqa: F821
        print(f"firmware version: {version}")
    else:
        print("firmware version: git could not say; Config.h's default applies")
