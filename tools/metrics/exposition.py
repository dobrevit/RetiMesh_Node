#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""The Prometheus text exposition format, written out by hand.

There is a client library for this, and it is not used, for one reason that
matters here: this exporter's whole job is to leave series *out*. A reading a
board cannot take must be an absent series, and a node that has gone quiet must
stop publishing a battery percentage rather than hold the last one flat
forever. The library is built around a registry of gauges that always have a
value, and the shape that fits this is a document assembled per scrape.

Hand-writing it also keeps the image to the two dependencies it already needs
for the radio, and makes the whole of the output a pure function of the state —
which is what lets CI check it with nothing installed.

The format is text/plain; version=0.0.4:

    # HELP <name> <text>
    # TYPE <name> gauge
    <name>{label="value"} <number>

Every sample of one metric must be grouped under a single HELP/TYPE pair, so
samples are collected into families and the document is rendered at the end
rather than streamed as the caller thinks of things.
"""

from __future__ import annotations

import math
import re

CONTENT_TYPE = "text/plain; version=0.0.4; charset=utf-8"

_NAME = re.compile(r"^[a-zA-Z_:][a-zA-Z0-9_:]*$")
_LABEL = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_]*$")


def _escape_label(value):
    return (str(value).replace("\\", "\\\\").replace('"', '\\"')
            .replace("\n", "\\n"))


def _escape_help(text):
    return str(text).replace("\\", "\\\\").replace("\n", "\\n")


def _number(value):
    """A float as Prometheus spells it.

    Python writes the three special values in lower case and Prometheus will
    not read them back, which is the kind of thing that produces an exporter
    that works until the first board reports a NaN RSSI.
    """
    value = float(value)
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "+Inf" if value > 0 else "-Inf"
    return repr(value)


class Exposition:
    """Samples in, one document out.

    Names are checked as they arrive rather than at render time: a malformed
    name makes Prometheus reject the *whole* scrape, so the failure would be a
    fleet that vanishes rather than one metric that misbehaves, and the stack
    trace at the point of the typo is worth having.
    """

    def __init__(self):
        self._families = {}     # name -> {"type", "help", "samples": {key: (labels, value)}}

    @staticmethod
    def _key(labels):
        """A series' identity: its labels, as they will be rendered.

        Values are coerced to text here rather than at render time, so that two
        labels which render the same are the same series — and so that ordering
        a document never compares a string against a number and raises inside a
        scrape.
        """
        return tuple(sorted((k, str(v)) for k, v in labels.items()))

    def _family(self, name, kind, help_text):
        if not _NAME.match(name):
            raise ValueError("not a Prometheus metric name: %r" % name)
        if kind == "counter" and not name.endswith("_total"):
            raise ValueError("a counter is named <thing>_total: %r" % name)
        fam = self._families.get(name)
        if fam is None:
            fam = self._families[name] = {"type": kind, "help": help_text, "samples": {}}
        elif fam["type"] != kind:
            raise ValueError("%s is already a %s" % (name, fam["type"]))
        return fam

    def sample(self, name, kind, help_text, value, labels=None):
        """One sample. Writing the same name and labels twice keeps the last.

        Two samples of one series in a single document is not a duplicate
        reading, it is a scrape Prometheus rejects outright — so the whole
        fleet vanishes rather than one metric misbehaving. It happens for an
        ordinary reason: two console commands can carry the same fact, and both
        POWER and STATUS report uptime. Whoever writes last wins, and callers
        that care order themselves so the freshest is last.
        """
        labels = dict(labels or {})
        for key in labels:
            if not _LABEL.match(key):
                raise ValueError("not a Prometheus label name: %r" % key)
        self._family(name, kind, help_text)["samples"][self._key(labels)] = (
            labels, value)

    def gauge(self, name, help_text, value, labels=None):
        self.sample(name, "gauge", help_text, value, labels)

    def counter(self, name, help_text, value, labels=None):
        self.sample(name, "counter", help_text, value, labels)

    def info(self, name, help_text, labels=None):
        """The join-me-later pattern: a constant 1 carrying labels.

        Strings are not values in Prometheus, so a node's firmware version and
        board live here and a dashboard brings them alongside the numbers with
        `... * on(node) group_left(board) retimesh_node_info`. Putting them on
        every metric instead would mean every series changed identity the day a
        node was upgraded, breaking the very graph that was meant to show it.
        """
        self.sample(name, "gauge", help_text, 1, labels)

    def render(self):
        out = []
        for name in sorted(self._families):
            fam = self._families[name]
            if not fam["samples"]:
                continue
            out.append("# HELP %s %s" % (name, _escape_help(fam["help"])))
            out.append("# TYPE %s %s" % (name, fam["type"]))
            # Sorted so a scrape diffed against the last one shows what moved
            # rather than what was iterated in a different order.
            for key, (_labels, value) in sorted(fam["samples"].items()):
                rendered = ",".join('%s="%s"' % (k, _escape_label(v)) for k, v in key)
                out.append("%s%s %s" % (name, "{%s}" % rendered if rendered else "",
                                        _number(value)))
        return "\n".join(out) + "\n" if out else ""
