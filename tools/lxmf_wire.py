#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""What a node's answers look like on the wire, in one place.

Two tools read a node: the soak collector, which keeps every answer, and the
metrics exporter, which turns the last one into gauges. They ask the same
questions over the same two channels, so the knowledge of what comes back
belongs to neither of them.

That is not tidiness. The sensor ids, the fixed-point scales and the array
positions here are a copy of src/rns/Telemetry.h — a copy in another language,
which no compiler will ever check against the original. One copy that both
tools read can be pinned to bytes the firmware actually produced (see
tools/tests/test_lxmf_wire.py, whose vectors come from the encoder itself).
Two copies drift, and the way telemetry drift shows up is not an error: a
sensor the firmware added is silently missing from a dashboard, or a scale
that moved reads as a node that walked half a degree east.

Nothing here imports RNS, so it is host-testable with nothing installed —
which is the whole reason the decoding lives on this side of the line and the
radio work lives in lxmf_fleet.py.
"""

from __future__ import annotations

import collections
import os
import re

# LXMF fields and command ids, from src/rns/LxmfFormat.h and Telemetry.h. They
# are Sideband's numbers rather than ours; the node follows them so that an
# ordinary client can ask the same questions without knowing about these tools.
FIELD_TELEMETRY = 0x02
FIELD_COMMANDS = 0x09
COMMAND_TELEMETRY = 0x01

# Sensor ids inside a telemetry document, and the names they are written under.
SENSORS = {
    0x01: "time",
    0x02: "location",
    0x04: "battery",
    0x05: "physical_link",
    0x0F: "information",
    0x13: "processor",
    0x14: "ram",
    0x15: "storage",
}

# Storage is the one sensor that carries more than one entry, and the label in
# front of each says which part it is (Telemetry.h: flash never moves, the card
# fills). An unknown label keeps its number rather than being guessed at.
STORAGE_PARTS = {0: "flash", 1: "card"}

# "RetiMesh Node v0.0.10-117-gbe85db0-dirty (Elecrow ThinkNode M9)" — built in
# RnsTransport.cpp and the only place a node says its version and its board.
_INFORMATION = re.compile(r"^RetiMesh Node\s+(?P<version>\S+)\s+\((?P<board>.+)\)\s*$")

# A maintenance console reply: "RM <CMD> key=value key=\"value with spaces\"".
# The final "RM OK <CMD>" and "RM ERR <CMD> <code> <text>" lines are answers
# about the request rather than readings, and are recognised so they are not
# mistaken for data (MaintenanceProtocol.h).
_RM_PAIR = re.compile(r'([A-Za-z0-9_.\-]+)=("(?:[^"\\]|\\.)*"|\S*)')


def env_list(name):
    """A comma-separated environment variable as a list, blanks discarded.

    Node addresses are the fleet's, not the repository's. Passing them as
    arguments in a compose file means they are committed with it — which is how
    a set of real device addresses ended up in this repository once already.
    An .env file the compose reads and git ignores keeps the deployment's own
    facts out of the deployment's source.
    """
    return [part.strip() for part in os.environ.get(name, "").split(",") if part.strip()]


def _default_unpack():
    """msgpack, from whatever is installed, or None.

    RNS vendors its own and every deployment of these tools has RNS, so this
    almost always finds one. It is looked up lazily rather than imported at the
    top because this module must stay importable with nothing installed.
    """
    try:
        from RNS.vendor import umsgpack
        return umsgpack.unpackb
    except Exception:
        pass
    try:
        import msgpack
        return lambda raw: msgpack.unpackb(raw, raw=False, strict_map_key=False)
    except Exception:
        return None


def unwrap_document(doc, unpack=None):
    """A telemetry field as a sensor map, in whichever of the two shapes it came.

    The field carries the same document two ways and both are the wire: this
    repo's encoder writes the map inline, and Sideband packs it to bytes and
    ships it as msgpack bin. src/rns/Telemetry.h accepts both on the way in for
    exactly this reason, and a reader that takes only the inline form silently
    drops every reading a phone volunteers.

    Returns None for anything that is not a sensor map, so a caller can tell
    "undecodable" from "decoded, and empty".
    """
    if isinstance(doc, (bytes, bytearray, memoryview)):
        loads = unpack or _default_unpack()
        if loads is None:
            return None
        try:
            doc = loads(bytes(doc))
        except Exception:
            return None
    return doc if isinstance(doc, dict) else None


def decode_telemetry(doc, unpack=None):
    """A readings map into something a later reader can use without this file.

    Sensor values keep the shapes the node sends — the arrays are Sideband's,
    not ours, and rewriting them here would be a second definition of the
    format that could drift from the firmware's. The two that are worth naming
    are unpacked because their shape is a nested pair nobody should have to
    remember: processor and RAM are [[label, [capacity, used]]].

    This is the shape the soak collector writes to its JSONL, so it is a file
    format as much as a return value. `normalise` below is the other reading of
    the same document, for callers that want numbers rather than the wire.
    """
    out = {}
    doc = unwrap_document(doc, unpack)
    if doc is None:
        return out
    for sid, value in doc.items():
        name = SENSORS.get(sid, "sensor_0x%02x" % sid) if isinstance(sid, int) else str(sid)
        if name in ("ram", "processor") and isinstance(value, (list, tuple)) and value:
            try:
                _label, pair = value[0]
                capacity, used = pair
                out[name] = {"capacity": capacity, "used": used,
                             "free": capacity - used}
                continue
            except Exception:
                pass                       # an unexpected shape is kept as it came
        out[name] = value
    return out


# --- the same document, read as numbers ---------------------------------------
#
# Everything below turns the wire shapes into named quantities in their proper
# units. The rule throughout is the firmware's own: a reading the node could
# not take is *absent*, never a zero. A missing key here becomes a missing
# series at the far end, which is the only representation of "this board cannot
# answer that" that a time series has. Filling it with 0 would put a flat line
# on a dashboard where there is no measurement at all, and a flat line is read
# as a working sensor.

def _num(value):
    """A finite number, or None. Booleans are not numbers here."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value if value == value and value not in (float("inf"), float("-inf")) else None


def _fixed(raw, width, scale, signed=True):
    """A big-endian fixed-point integer of exactly `width` bytes, or None.

    The width test is the whole safety of this: src/rns/Telemetry.h refuses a
    coordinate of the wrong width rather than reading it as a different number,
    because no data beats wrong data and a mis-scaled latitude is a node that
    appears in the sea.
    """
    if not isinstance(raw, (bytes, bytearray, memoryview)):
        return None
    raw = bytes(raw)
    if len(raw) != width:
        return None
    return int.from_bytes(raw, "big", signed=signed) / scale


def _entries(value):
    """The [[label, [capacity, used]], ...] shape the three resource sensors share."""
    out = []
    if not isinstance(value, (list, tuple)):
        return out
    for item in value:
        try:
            label, pair = item
            capacity, used = pair
        except Exception:
            continue
        if _num(capacity) is None or _num(used) is None:
            continue
        out.append((label, capacity, used))
    return out


def parse_information(text):
    """A node's self-description into its version and its board, or (None, None).

    Built by RnsTransport.cpp as "RetiMesh Node <version> (<board>)". Anything
    else is left alone: unsolicited telemetry arrives from Sideband too, and a
    phone's information line is not this and must not be forced into it.
    """
    if not isinstance(text, str):
        return None, None
    m = _INFORMATION.match(text.strip())
    if not m:
        return None, None
    return m.group("version"), m.group("board")


def normalise(doc, unpack=None):
    """A telemetry document as named quantities in SI-ish units.

    Absent readings are absent keys. `storage` is a dict keyed by part name
    because a node that keeps its store on a card has two, and reporting only
    the internal flash showed an operator a figure that could not change.
    """
    r = {}
    doc = unwrap_document(doc, unpack)
    if doc is None:
        return r

    for sid, value in doc.items():
        name = SENSORS.get(sid) if isinstance(sid, int) else None

        if name == "time":
            # The node's own clock. Kept as it comes, including a wrong one: a
            # node with no GNSS fix and no RTC stamps 1970, and the way to see
            # that is to plot it against the collector's clock rather than to
            # have this quietly drop it.
            if _num(value) is not None:
                r["clock_seconds"] = float(value)

        elif name == "information":
            if isinstance(value, bytes):
                value = value.decode("utf-8", "replace")
            if isinstance(value, str) and value:
                r["information"] = value
                version, board = parse_information(value)
                if version:
                    r["firmware_version"] = version
                if board:
                    r["board"] = board

        elif name == "battery":
            # [percent, charging, temperature]. Charging is nil where the board
            # cannot see its charger, and that nil has to survive: a false here
            # reads as "plugged in and not taking charge", which sends somebody
            # looking for a fault in a working cable (src/sys/Power.h).
            if isinstance(value, (list, tuple)) and value:
                if _num(value[0]) is not None:
                    r["battery_percent"] = float(value[0])
                if len(value) > 1 and isinstance(value[1], bool):
                    r["battery_charging"] = value[1]
                if len(value) > 2 and _num(value[2]) is not None:
                    r["battery_temperature_celsius"] = float(value[2])

        elif name == "location":
            r.update(_location(value))

        elif name == "physical_link":
            # [rssi, snr, quality] — what the node heard of the message that
            # asked it, not a property of the node.
            if isinstance(value, (list, tuple)):
                for key, i in (("rssi_dbm", 0), ("snr_db", 1), ("link_quality_percent", 2)):
                    if len(value) > i and _num(value[i]) is not None:
                        r[key] = float(value[i])

        elif name == "processor":
            # Packed as a capacity with nothing used, because the shape is
            # shared with RAM and storage. The capacity is the clock.
            for _label, capacity, _used in _entries(value)[:1]:
                r["cpu_clock_hertz"] = float(capacity)

        elif name == "ram":
            # Internal memory, not the total including PSRAM. That distinction
            # is the point of collecting it: on a board with 8 MB of PSRAM the
            # total looks healthy right up until the node dies of the kind it
            # actually needs.
            for _label, capacity, used in _entries(value)[:1]:
                r["ram_capacity_bytes"] = float(capacity)
                r["ram_used_bytes"] = float(used)

        elif name == "storage":
            parts = {}
            for label, capacity, used in _entries(value):
                part = STORAGE_PARTS.get(label, str(label))
                parts[part] = {"capacity_bytes": float(capacity),
                               "used_bytes": float(used)}
            if parts:
                r["storage"] = parts

    return r


def _location(value):
    """The position array, positionally, refusing anything of the wrong width.

    [latitude, longitude, altitude, speed, bearing, accuracy, taken_at], all
    big-endian fixed point as msgpack bin except the timestamp. A coordinate
    that is not exactly four bytes is not a coordinate, and the whole position
    is dropped rather than half-read — the same rule the firmware's own
    parsePosition applies to what arrives.
    """
    out = {}
    if not isinstance(value, (list, tuple)) or len(value) < 2:
        return out
    lat = _fixed(value[0], 4, 1e6)
    lon = _fixed(value[1], 4, 1e6)
    if lat is None or lon is None:
        return out
    out["latitude_degrees"] = lat
    out["longitude_degrees"] = lon

    tail = (("altitude_meters", 2, 4, 1e2, True),
            ("speed_kmh", 3, 4, 1e2, False),
            ("bearing_degrees", 4, 4, 1e2, True),
            ("accuracy_meters", 5, 2, 1e2, False))
    for key, i, width, scale, signed in tail:
        if len(value) > i:
            got = _fixed(value[i], width, scale, signed)
            if got is not None:
                out[key] = got
    # The sender's clock when the fix was taken. Zero is "never", not 1970.
    if len(value) > 6 and _num(value[6]) and value[6] > 0:
        out["position_timestamp_seconds"] = float(value[6])
    return out


# --- the console channel ------------------------------------------------------

# What one "RM ..." line turned out to be. The three are kept apart because
# they mean different things to a caller: data carries readings, ok closes a
# reply, and err is the node refusing — and a refusal is the opposite of
# silence. "RM ERR ADMIN 403" is what an un-enrolled collector gets for every
# console request it ever makes, and a reader that folded that into "no data"
# would show an operator an empty graph with nothing to explain it.
ConsoleLine = collections.namedtuple("ConsoleLine", "command kind fields")


def parse_console_line(line):
    """One "RM ..." reply line as a ConsoleLine, or None if it is not one.

    Log lines share the port with replies, which is why every reply begins with
    "RM " in the first place (src/sys/MaintenanceProtocol.h). Anything that
    does not is somebody else's output and is left alone.

    Field values are strings. What they mean is the caller's business: this
    file knows the shape of a line, not which keys are bytes and which are
    seconds.
    """
    if not isinstance(line, str):
        return None
    line = line.strip()
    if not line.startswith("RM "):
        return None
    rest = line[3:].strip()
    if not rest:
        return None
    head, _, tail = rest.partition(" ")
    head = head.upper()

    if head == "OK":
        # "RM OK STACKS lines=3" — the command is the next word, and anything
        # after it is about the reply rather than about the node.
        cmd, _, tail = tail.strip().partition(" ")
        return ConsoleLine(cmd.upper(), "ok", _pairs(tail)) if cmd else None

    if head == "ERR":
        # "RM ERR <CMD> <code> <text>", the codes borrowed from HTTP.
        cmd, _, tail = tail.strip().partition(" ")
        code, _, text = tail.strip().partition(" ")
        if not cmd:
            return None
        return ConsoleLine(cmd.upper(), "err",
                           {"code": code, "text": text.strip()})

    return ConsoleLine(head, "data", _pairs(tail))


def _pairs(text):
    out = {}
    for key, raw in _RM_PAIR.findall(text or ""):
        if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
            raw = raw[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        out[key] = raw
    return out
