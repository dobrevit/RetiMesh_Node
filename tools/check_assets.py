#!/usr/bin/env python3
"""Refuse to build a portal page whose JavaScript does not parse.

A syntax error in one of these files is not a small bug. The browser stops at
the first one and runs *none* of the script, so the page renders its markup and
then does nothing: no fetch of /api/settings, no region list, no frequency
list, no station fields — a settings page that looks like a settings page and
is inert. Nothing else catches it. The firmware compiles, the image builds, the
asset stamp matches, the node serves the file byte for byte, and every check
this project had said the two halves agreed. They did. The page was broken
before it ever reached the node.

It happened: two adjacent string literals with no `+` between them, which is
how C++ concatenates and is a syntax error in JavaScript. It was written in a
C++ file's neighbourhood by somebody with C++ in their fingers, and it went out
to the whole fleet.

So the build asks node to parse each <script> block, and stops if one does not.
`node --check` is a parse, not a run: it costs milliseconds and needs nothing
installed beyond node itself. Where node is missing the check says so and lets
the build through rather than blocking somebody who cannot fix it — CI has
node, so a page like that cannot reach main even if it can reach one bench.

Run standalone as well:  python3 tools/check_assets.py
"""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = re.compile(r"<script[^>]*>(.*?)</script>", re.S)


def script_blocks(html: str) -> list:
    """Every inline <script> body, in order. Blocks with a src= are skipped by
    the pattern only if they are empty, which is what we want: there is nothing
    of ours to parse in one that points elsewhere."""
    return [b for b in SCRIPT.findall(html) if b.strip()]


def check_file(path: Path, node: str) -> list:
    """Parse errors in one page, as printable lines. Empty when it is fine."""
    blocks = script_blocks(path.read_text(encoding="utf-8"))
    if not blocks:
        return []
    # One temporary file per page rather than per block: the blocks share a
    # scope in the browser, so a helper defined in one and used in another is
    # not an error there and must not be one here.
    with tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8", delete=False) as fh:
        fh.write("\n".join(blocks))
        tmp = fh.name
    try:
        done = subprocess.run([node, "--check", tmp], capture_output=True, text=True)
        if done.returncode == 0:
            return []
        # node names the temporary file; say the page instead, since that is
        # the file somebody has to open.
        return [ln.replace(tmp, str(path)) for ln in done.stderr.strip().splitlines()[:6]]
    finally:
        Path(tmp).unlink(missing_ok=True)


def check(data_dir: Path, log=print) -> bool:
    node = shutil.which("node")
    pages = sorted(data_dir.glob("*.html"))
    if not pages:
        return True
    if not node:
        log("assets: node not found, so the pages' JavaScript was not parsed "
            "(CI does parse it; install node to catch a syntax error here)")
        return True
    bad = False
    for page in pages:
        problems = check_file(page, node)
        if problems:
            bad = True
            log("assets: %s does not parse — the browser will run none of its "
                "script, so the page will load and do nothing:" % page)
            for line in problems:
                log("  " + line)
    return not bad


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    return 0 if check(root / "data") else 1


# PlatformIO runs this as a pre-action; standalone it is a plain script. Under
# SCons the module is exec'd without __file__, so the data directory comes from
# the build environment the way tools/asset_stamp.py takes it.
try:
    Import("env")  # noqa: F821  (injected by PlatformIO)
except NameError:
    if __name__ == "__main__":
        sys.exit(main())
else:
    if not check(Path(env.subst("$PROJECT_DATA_DIR"))):  # noqa: F821
        raise SystemExit("assets: a portal page's JavaScript does not parse; refusing to build")
