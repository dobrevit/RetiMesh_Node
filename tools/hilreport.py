# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""The PASS/FAIL line every HIL script prints, and the count it exits with.

Two scripts had grown identical nested copies of this. It is four lines, which
is exactly the size of thing that drifts: one gains a timestamp, the other a
colour, and a runner that greps for the line breaks on one of them.
"""


class Reporter:
    def __init__(self):
        self.fails = 0

    def __call__(self, name: str, ok: bool, detail: str = "") -> None:
        self.fails += 0 if ok else 1
        print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")
