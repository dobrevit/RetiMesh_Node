#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Render the supported-boards matrix in docs/hardware.md from boards.json.

The registry is where a board's facts live — CI, the release packager, the
web flasher and the CLI all read it — and the hardware page used to carry a
second copy of the same facts as a table written by hand, which is the copy
that drifts. This writes the table between the two markers in the page, and
`--check` (run by CI) fails when the page no longer matches the registry, so
the fix is always to edit boards.json and re-run this.

    python tools/board_docs.py          # rewrite the table in place
    python tools/board_docs.py --check  # exit 1 if docs/hardware.md is stale
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = ROOT / "boards.json"
PAGE = ROOT / "docs" / "hardware.md"
BEGIN = "<!-- boards.json:begin -->"
END = "<!-- boards.json:end -->"
NOTE = "<!-- Rendered by tools/board_docs.py from boards.json. Edit the registry, not this table. -->"
COLUMNS = [("Env", None), ("Board", "name"), ("MCU", "mcu"), ("Radio", "radio"),
           ("Display", "display"), ("Extras", "extras"), ("Status", "status")]


def render(boards: dict) -> str:
    rows = ["| " + " | ".join(h for h, _ in COLUMNS) + " |", "|" + "---|" * len(COLUMNS)]
    for env, b in boards.items():
        if env.startswith("_"):
            continue
        missing = [k for _, k in COLUMNS if k and k not in b]
        if missing:
            sys.exit(f"boards.json: {env} lacks {', '.join(missing)}")
        cells = [f"`{env}`"] + [str(b[k]).replace("|", "\\|") for _, k in COLUMNS if k]
        rows.append("| " + " | ".join(cells) + " |")
    return "\n".join(rows)


def splice(page: str, table: str) -> str:
    try:
        head, rest = page.split(BEGIN + "\n", 1)
        _, tail = rest.split(END, 1)
    except ValueError:
        sys.exit(f"{PAGE.relative_to(ROOT)}: markers {BEGIN} / {END} not found")
    return head + BEGIN + "\n" + NOTE + "\n" + table + "\n" + END + tail


def main() -> int:
    check = "--check" in sys.argv[1:]
    boards = json.loads(REGISTRY.read_text())
    before = PAGE.read_text()
    after = splice(before, render(boards))
    if after == before:
        print(f"{PAGE.relative_to(ROOT)}: board matrix matches boards.json")
        return 0
    if check:
        print(f"{PAGE.relative_to(ROOT)}: board matrix is stale — run tools/board_docs.py", file=sys.stderr)
        return 1
    PAGE.write_text(after)
    print(f"{PAGE.relative_to(ROOT)}: board matrix rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
