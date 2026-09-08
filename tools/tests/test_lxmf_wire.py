# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.
#
# What a node's telemetry means, checked against bytes the node's own encoder
# produced.
#
# lxmf_wire.py is a copy of src/rns/Telemetry.h in another language, and no
# compiler will ever check one against the other. The way that drift shows up
# is not an error message: a sensor the firmware added is quietly missing from
# every dashboard, or a scale that moved reads as a node that walked half a
# degree east. So the fixtures below are not hand-written — they are what
# Rns::Telemetry::build() actually emitted for two known snapshots, and the
# regeneration command is in GOLDEN_* below so the next person can redo it.
#
# One rule is worth the whole file: **absent is not zero**. A board that cannot
# see its charger sends nil rather than false, because "not charging" sends
# somebody looking for a fault in a working cable. A node whose clock was never
# set sends no time at all rather than 1970. Every one of those distinctions is
# one `.get(key, 0)` away from being destroyed, and nothing downstream would
# notice: it would just draw a confident, wrong line.

import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.join(_HERE, os.pardir)

_spec = importlib.util.spec_from_file_location(
    "lxmf_wire", os.path.join(_TOOLS, "lxmf_wire.py"))
wire = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(wire)


# The two documents below came out of the firmware itself. To make them again:
#
#     g++ -std=c++17 -I src/rns -o /tmp/emit emit.cpp && /tmp/emit f
#
# where emit.cpp calls Rns::Telemetry::build() on the snapshots described here.
# The first is test/test_telemetry/test_main.cpp's full() plus a card, so the
# storage list has both of its parts; the second is a board with no clock, no
# charger it can see, no card and a position below sea level.
GOLDEN_FULL_HEX = (
    "8801ce6955b9000fd923526574694d657368204e6f64652076302e312e302028"
    "4c696c79474f2054332d5333290493cb4055e00000000000c3c00297c404028b"
    "83e4c4040163dd2cc4040000e86cc40400000000c40400000000c40202eece69"
    "55b8f60593cbc05a000000000000cb40218000000000003e1391920092ce0e4e"
    "1c00001491920092ce00050000ce00037b441592920092ce00300000ce001aba"
    "6e920192cf00000003a3648000ce1ade8000")
GOLDEN_SPARSE_HEX = (
    "840fd92c526574694d657368204e6f64652076302e312e30202848656c746563"
    "20576972656c657373205061706572290493cb4044800000000000c0c00297c4"
    "04030fc738c40400015663c404fffff7fec40400000145c40400000000c40201"
    "4e001491920092ce00044978ce00035e44")

# The same two documents as the objects an unpacker yields, so these tests run
# with nothing installed — which is what CI has. The raw bytes above are read
# back as well wherever msgpack happens to be available, which is what pins
# this transcription to them.
FULL = {
    1: 1767225600,
    2: [b"\x02\x8b\x83\xe4", b"\x01c\xdd,", b"\x00\x00\xe8l",
        b"\x00\x00\x00\x00", b"\x00\x00\x00\x00", b"\x02\xee", 1767225590],
    4: [87.5, True, None],
    5: [-104.0, 8.75, 62],
    15: "RetiMesh Node v0.1.0 (LilyGO T3-S3)",
    19: [[0, [240000000, 0]]],
    20: [[0, [327680, 228164]]],
    21: [[0, [3145728, 1751662]], [1, [15626174464, 450789376]]],
}
SPARSE = {
    2: [b"\x03\x0f\xc78", b"\x00\x01Vc", b"\xff\xff\xf7\xfe",
        b"\x00\x00\x01E", b"\x00\x00\x00\x00", b"\x01N", 0],
    4: [41.0, None, None],
    15: "RetiMesh Node v0.1.0 (Heltec Wireless Paper)",
    20: [[0, [280952, 220740]]],
}


def _unpacker():
    """msgpack from wherever it is, or None. CI has neither; a deployment has RNS."""
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


class TheGoldenVectorsAreWhatTheFirmwareEmitted(unittest.TestCase):
    """The transcriptions above are the bytes below, or these tests prove nothing."""

    def setUp(self):
        self.unpack = _unpacker()
        if self.unpack is None:
            # Skipping here is fine on a bench and not fine in CI: these three
            # are the only tests that tie the golden hex to the dictionaries
            # every other test asserts against, so a run without them cannot
            # see the two drift apart. CI sets this and gets a failure instead.
            if os.environ.get("RETIMESH_REQUIRE_UNPACKER"):
                self.fail("RETIMESH_REQUIRE_UNPACKER is set and no msgpack is "
                          "installed: the golden vectors would go unchecked")
            self.skipTest("no msgpack available; the decoded fixtures still run")

    def test_the_full_document_unpacks_to_the_fixture(self):
        self.assertEqual(self.unpack(bytes.fromhex(GOLDEN_FULL_HEX)), FULL)

    def test_the_sparse_document_unpacks_to_the_fixture(self):
        self.assertEqual(self.unpack(bytes.fromhex(GOLDEN_SPARSE_HEX)), SPARSE)

    def test_raw_bytes_are_accepted_as_well_as_a_map(self):
        # Sideband packs the document to bytes and ships it as msgpack bin;
        # this repo's encoder writes the map inline. Both are the wire, and a
        # reader that takes only one silently drops everything a phone sends.
        self.assertEqual(wire.normalise(bytes.fromhex(GOLDEN_FULL_HEX)),
                         wire.normalise(FULL))


class ANodeWithEverything(unittest.TestCase):
    def setUp(self):
        self.r = wire.normalise(FULL)

    def test_the_clock_survives(self):
        self.assertEqual(self.r["clock_seconds"], 1767225600.0)

    def test_the_position_is_fixed_point_not_a_float(self):
        # Packed as big-endian fixed point in msgpack bin, scaled by 1e6 for
        # coordinates and 1e2 for the rest. Getting the scale wrong here puts a
        # node in the sea, and nothing else in the system would object.
        self.assertAlmostEqual(self.r["latitude_degrees"], 42.6977, places=6)
        self.assertAlmostEqual(self.r["longitude_degrees"], 23.3219, places=6)
        self.assertAlmostEqual(self.r["altitude_meters"], 595.0, places=2)
        self.assertAlmostEqual(self.r["accuracy_meters"], 7.5, places=2)
        self.assertEqual(self.r["position_timestamp_seconds"], 1767225590.0)

    def test_the_board_and_version_come_out_of_the_information_line(self):
        self.assertEqual(self.r["board"], "LilyGO T3-S3")
        self.assertEqual(self.r["firmware_version"], "v0.1.0")

    def test_battery_and_signal(self):
        self.assertEqual(self.r["battery_percent"], 87.5)
        self.assertIs(self.r["battery_charging"], True)
        self.assertEqual(self.r["rssi_dbm"], -104.0)
        self.assertEqual(self.r["snr_db"], 8.75)
        self.assertEqual(self.r["link_quality_percent"], 62.0)

    def test_the_nested_resource_shape_is_unpacked(self):
        # [[label, [capacity, used]]] for all three, and the processor's
        # capacity is the clock rather than an amount of anything.
        self.assertEqual(self.r["cpu_clock_hertz"], 240000000.0)
        self.assertEqual(self.r["ram_capacity_bytes"], 327680.0)
        self.assertEqual(self.r["ram_used_bytes"], 228164.0)

    def test_storage_keeps_both_parts_apart(self):
        # A node keeping its store on a card has an internal flash that never
        # moves and a card that fills; reporting only the first showed an
        # operator a figure that could not change.
        self.assertEqual(sorted(self.r["storage"]), ["card", "flash"])
        self.assertEqual(self.r["storage"]["flash"]["capacity_bytes"], 3145728.0)
        self.assertEqual(self.r["storage"]["card"]["used_bytes"], 450789376.0)


class ANodeThatCannotAnswerEverything(unittest.TestCase):
    """The point of the sparse vector: every missing reading stays missing."""

    def setUp(self):
        self.r = wire.normalise(SPARSE)

    def test_a_charger_the_board_cannot_see_is_absent_not_false(self):
        self.assertIn("battery_percent", self.r)
        self.assertNotIn("battery_charging", self.r)

    def test_a_clock_that_was_never_set_is_absent_not_1970(self):
        self.assertNotIn("clock_seconds", self.r)

    def test_sensors_the_board_does_not_have_are_absent_not_zero(self):
        for key in ("rssi_dbm", "snr_db", "cpu_clock_hertz", "storage"):
            self.assertNotIn(key, self.r)

    def test_a_fix_with_no_timestamp_still_gives_a_position(self):
        # positionAt is 0 when the node has no clock to stamp it with. The
        # coordinates are still real, so the position survives and only the
        # timestamp is dropped.
        self.assertAlmostEqual(self.r["latitude_degrees"], 51.365688, places=6)
        self.assertNotIn("position_timestamp_seconds", self.r)

    def test_a_negative_altitude_keeps_its_sign(self):
        # Altitude is the signed field, and a wrong width or an unsigned read
        # puts a node in orbit rather than below sea level.
        self.assertAlmostEqual(self.r["altitude_meters"], -20.5, places=2)


class ADocumentThatIsNotOne(unittest.TestCase):
    def test_junk_decodes_to_nothing_rather_than_raising(self):
        # This runs in a delivery callback on RNS's own thread. An exception
        # there is a collector that stops collecting, so a malformed document
        # from a stranger has to be a quiet empty answer.
        for junk in (None, b"", b"\xc1\xc1\xc1", 42, "not a document", []):
            self.assertEqual(wire.normalise(junk), {})
            self.assertEqual(wire.decode_telemetry(junk), {})

    def test_a_coordinate_of_the_wrong_width_drops_the_whole_position(self):
        # No data beats wrong data: the firmware's own parsePosition refuses a
        # coordinate that is not exactly four bytes rather than reading it as a
        # different number.
        doc = dict(SPARSE)
        doc[2] = [b"\x03\x0f\xc7", b"\x00\x01Vc"]
        r = wire.normalise(doc)
        self.assertNotIn("latitude_degrees", r)
        self.assertIn("battery_percent", r)          # the rest of it still arrives

    def test_a_storage_label_that_cannot_be_a_dict_key_is_dropped(self):
        # The label is looked up with dict.get(), and an array or a map label is
        # unhashable — so the lookup raises TypeError rather than missing, on
        # RNS's delivery thread, from a document any stranger can send. One bad
        # reading would stop the collector instead of being dropped.
        for label in ([1, 2], {"a": 1}, "flash", None, True):
            doc = dict(SPARSE)
            doc[0x15] = [[label, [100, 50]]]
            r = wire.normalise(doc)                    # must not raise
            self.assertNotIn("storage", r)
        good = dict(SPARSE)
        good[0x15] = [[1, [100, 50]]]
        self.assertEqual(sorted(wire.normalise(good)["storage"]), ["card"])

    def test_a_sensor_this_tool_does_not_know_is_ignored_not_guessed_at(self):
        doc = dict(SPARSE)
        doc[0x7E] = [1, 2, 3]
        self.assertEqual(wire.normalise(doc), wire.normalise(SPARSE))

    def test_an_unknown_sensor_still_survives_the_soak_collector_s_decoder(self):
        # decode_telemetry is a file format as well as a return value: the soak
        # collector writes it to JSONL, and a reading nobody has named yet is
        # worth keeping under its number rather than discarding.
        out = wire.decode_telemetry({0x7E: [1, 2, 3]})
        self.assertEqual(out, {"sensor_0x7e": [1, 2, 3]})


class TheInformationLine(unittest.TestCase):
    def test_a_node_says_its_version_and_its_board(self):
        self.assertEqual(
            wire.parse_information(
                "RetiMesh Node v0.0.10-117-gbe85db0-dirty (Elecrow ThinkNode M9)"),
            ("v0.0.10-117-gbe85db0-dirty", "Elecrow ThinkNode M9"))

    def test_somebody_else_s_information_is_left_alone(self):
        # Unsolicited telemetry arrives from Sideband too, and a phone's
        # information line is not this shape and must not be forced into it.
        self.assertEqual(wire.parse_information("Sideband on a Pixel"), (None, None))


class TheConsoleChannel(unittest.TestCase):
    def test_a_data_line_becomes_its_pairs(self):
        line = wire.parse_console_line("RM STACKS tightest=radio headroom=2048")
        self.assertEqual(line.command, "STACKS")
        self.assertEqual(line.kind, "data")
        self.assertEqual(line.fields, {"tightest": "radio", "headroom": "2048"})

    def test_a_quoted_value_keeps_its_spaces(self):
        line = wire.parse_console_line('RM STATUS reset="panic or unhandled exception"')
        self.assertEqual(line.fields["reset"], "panic or unhandled exception")

    def test_ok_closes_a_reply_and_carries_no_readings(self):
        line = wire.parse_console_line("RM OK STACKS lines=3")
        self.assertEqual((line.command, line.kind), ("STACKS", "ok"))

    def test_a_refusal_is_not_silence(self):
        # "RM ERR ADMIN 403" is what an un-enrolled collector gets for every
        # console request it ever makes. Folded into "no data" it would look
        # exactly like a node that never answers, and the operator would go
        # looking at the radio instead of at the administrator list.
        line = wire.parse_console_line("RM ERR ADMIN 403 not_an_administrator")
        self.assertEqual((line.command, line.kind), ("ADMIN", "err"))
        self.assertEqual(line.fields["code"], "403")

    def test_log_lines_sharing_the_port_are_not_replies(self):
        # Every reply begins with "RM " precisely so a host can pick them out
        # of the log lines that share the port.
        self.assertIsNone(wire.parse_console_line("I (5123) wifi: state change"))
        self.assertIsNone(wire.parse_console_line(""))


if __name__ == "__main__":
    unittest.main()
