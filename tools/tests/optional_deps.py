# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
#
# Tests that need something not every machine has, and the one rule about them.
#
# A few tests here can only run where a library is installed: RNS, or an
# msgpack. They are not incidental tests. They are the ones that tie a constant
# or a fixture in this repository to what the reference implementation actually
# produces — `RNS_HEADER_BYTES` against `RNS.Reticulum.TRUNCATED_HASHLENGTH`,
# the golden telemetry hex against the dictionaries every other assertion uses.
# Nothing else checks those. Nothing can: the whole point is that one side is a
# hand-written copy of the other.
#
# So skipping them is right on a bench, where a missing library is a fact about
# the laptop, and wrong in CI, where it is a green run that checked the thing it
# was built to check exactly zero times. Both of the tests this file serves were
# written to skip, and both would have skipped in CI forever.
#
# `RETIMESH_REQUIRE_DEPS=1` — set by the workflow, next to the pip install that
# provides them — turns the skip into a failure. One switch rather than one per
# library, because the rule is about CI and not about any particular import: a
# test added tomorrow that needs a fourth library gets the same treatment by
# calling `need()` and nothing else.

import importlib.util
import os

ENV = "RETIMESH_REQUIRE_DEPS"


def installed(module):
    """Whether an optional import would succeed, without importing it."""
    try:
        return importlib.util.find_spec(module) is not None
    except (ImportError, ValueError):
        return False


def need(case, module, why):
    """Skip this test on a bench where `module` is missing; fail in CI.

    `why` says what goes unchecked without it, because that sentence is the
    whole justification for the test existing and is what a person reading a
    CI failure needs in order to decide between installing the library and
    deleting the test.
    """
    if installed(module):
        return
    if os.environ.get(ENV):
        case.fail("%s is set and %s is not installed, so %s"
                  % (ENV, module, why))
    case.skipTest("%s is not installed here, so %s" % (module, why))
