# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Host-side tests for retimesh_flash.device with no hardware: fake ports, a
fake serial transport that speaks the maintenance protocol, a fake HTTP.

Run: python -m unittest discover -s tools/retimesh-flash/tests
"""
import unittest
from types import SimpleNamespace

from retimesh_flash import device
from retimesh_flash.device import (Console, HandOff, Port, esp_candidates, esptool_args, hand_off_to_bootloader,
                                   list_ports, node_url, parse_kv, probe_http, request_bootloader_http,
                                   select_port, wait_for_application)


def fake_comport(dev, vid=None, pid=None, serial=None, product=None, location=None):
    return SimpleNamespace(device=dev, vid=vid, pid=pid, serial_number=serial, product=product, location=location)


S3 = fake_comport("/dev/ttyACM0", 0x303A, 0x1001, "7C:DF:A1:12:34:56", "USB JTAG/serial debug unit", "1-3")
CP2102 = fake_comport("/dev/ttyUSB0", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-4")
CP2102_B = fake_comport("/dev/ttyUSB1", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-5")
LEGACY = fake_comport("/dev/ttyS0")
ARDUINO = fake_comport("/dev/ttyACM1", 0x2341, 0x0043, "555", "Arduino Uno")
FT2232 = fake_comport("/dev/ttyUSB2", 0x0403, 0x6010, "AB12", "FT2232H Dual UART")   # a known vendor, an unlisted product
# One chip, two faces: the composite device the application presents, and the
# serial-JTAG unit the ROM downloader comes up on — same MAC, spelt two ways.
COMPOSITE = fake_comport("/dev/ttyACM5", 0x1209, 0x0001, "1CDBD4821454", "RetiMesh Node", "1-11")
S3_ROM = fake_comport("/dev/ttyACM5", 0x303A, 0x1001, "1C:DB:D4:82:14:54", "USB JTAG/serial debug unit", "1-11")
COMPOSITE_INFO = device.NodeInfo("RetiMesh Node", "v0.3.0", "LilyGO T3-S3", "console:/dev/ttyACM5")
# A composite device with no usable serial (routine on Windows), and a
# serial-JTAG unit that is some other chip's.
COMPOSITE_NAMELESS = fake_comport("/dev/ttyACM5", 0x1209, 0x0001, None, "RetiMesh Node", "1-11")
OTHER_S3_ROM = fake_comport("/dev/ttyACM9", 0x303A, 0x1001, "AA:BB:CC:DD:EE:FF", "USB JTAG/serial debug unit", "1-12")


class FakeSerial:
    """Answers like the firmware's Maintenance.cpp, log lines included."""

    def __init__(self, board="LilyGO T3-S3", software_entry=True, silent=False,
                 drop_first=False, drop_always=False, counts=True, stale_prefix=False):
        self.board = board
        self.software_entry = software_entry
        self.silent = silent
        # The S3's habit: the first RM line after open goes missing. drop_always
        # is a port that never delivers a whole reply; counts=False is a node on
        # protocol 1, before OK lines carried the count.
        self.drop_first = drop_first
        self.drop_always = drop_always
        self.counts = counts
        # Bytes a port prober left in the node's line buffer: the first
        # command arrives glued to them and is refused under no name.
        self.stale_prefix = stale_prefix
        self.out = bytearray()
        self.sent = []
        self.dtr = self.rts = None

    def _ok(self, cmd, lines, kv=""):
        count = f" lines={lines}" if self.counts else ""
        return f"RM OK {cmd}{count}{' ' + kv if kv else ''}"

    def write(self, data):
        self.sent.append(data.decode())
        if self.silent:
            return
        line = data.decode().strip().upper()
        reply = ["[I][main.cpp:200] heartbeat line that shares the port"]
        if self.stale_prefix:
            self.stale_prefix = False
            reply += ["RM ERR ? 400 bad argument, try HELP"]
        elif line == "VERSION":
            reply += [f'RM VERSION firmware="RetiMesh Node" version=v0.2.0 board="{self.board}" idf=v4.4.7',
                      self._ok("VERSION", 1)]
        elif line == "BOOTLOADER CONFIRM":
            if self.software_entry:
                reply += [self._ok("BOOTLOADER", 0, "target=bootloader method=software_api delay_ms=300")]
            else:
                reply += ["RM ERR BOOTLOADER 501 this chip cannot enter its downloader from software"]
        elif line == "BOOTLOADER":
            reply += ["RM ERR BOOTLOADER 400 add CONFIRM: BOOTLOADER CONFIRM"]
        else:
            reply += [f"RM ERR {line.split()[0]} 404 unknown command, try HELP"]
        if self.drop_first or self.drop_always:
            first = next(i for i, l in enumerate(reply) if l.startswith("RM "))
            del reply[first]
            self.drop_first = False
        self.out += ("\n".join(reply) + "\n").encode()

    def flush(self):
        pass

    def read(self, n=1):
        if not self.out:
            return b""
        b = bytes(self.out[:n]); del self.out[:n]
        return b

    def close(self):
        pass


class Clock:
    def __init__(self):
        self.t = 0.0
    def now(self):
        return self.t
    def sleep(self, s):
        self.t += s


class PortsTest(unittest.TestCase):
    def test_kinds_and_ordering(self):
        ports = list_ports([LEGACY, ARDUINO, CP2102, S3])
        self.assertEqual([p.kind for p in ports], ["usb_serial_jtag", "CP2102", "unknown", "legacy"])
        self.assertTrue(ports[0].auto_reset)
        self.assertTrue(ports[1].auto_reset)
        self.assertFalse(ports[2].auto_reset)

    def test_the_composite_device_is_a_node_esptool_must_not_reset(self):
        ports = list_ports([COMPOSITE, S3])
        comp = ports[[p.kind for p in ports].index("retimesh_composite")]
        self.assertTrue(comp.likely_esp)
        self.assertFalse(comp.auto_reset)
        # The identity is the registry's, not a second opinion.
        import json, pathlib
        ident = json.loads((pathlib.Path(__file__).resolve().parents[3] / "boards.json").read_text())["_usb_identity"]
        self.assertEqual(device.RETIMESH_COMPOSITE, (int(ident["vid"], 16), int(ident["pid"], 16)))
        self.assertEqual(comp.product, ident["product"])

    def test_both_faces_of_one_chip_share_a_node_id(self):
        comp, rom = list_ports([COMPOSITE]), list_ports([S3_ROM])
        self.assertEqual(comp[0].node_id, rom[0].node_id)
        self.assertEqual(comp[0].node_id, "1CDBD4821454")

    def test_the_usb_address_follows_the_firmware_rule(self):
        # LocalLinkState.h: 10.64.<last MAC octet>.1 — 0x54 is 84 — and the
        # PPP link one subnet over, 10.65, from the same byte.
        self.assertEqual(device.usb_node_url("1CDBD4821454"), "http://10.64.84.1")
        self.assertEqual(device.ppp_node_url("1C:DB:D4:82:14:54"), "http://10.65.84.1")
        # Anything that is not a MAC names no chip and no address: not a
        # crash, and not an address on somebody else's subnet.
        for bad in ("RETIMESH", "0001", None, "", "1CDBD48214"):
            self.assertIsNone(device.usb_node_url(bad), bad)
        self.assertIsNone(list_ports([fake_comport("/dev/ttyACM7", 0x1209, 0x0001, "RETIMESH", "RetiMesh Node")])[0].node_id)

    def test_a_known_vendor_with_an_unlisted_product_is_still_a_candidate(self):
        # An FT2232 devkit is not in the bridge table, but nobody puts one in
        # front of anything other than an ESP32 on this bench; it must not be
        # dropped from auto-selection the way an Arduino is.
        ports = list_ports([FT2232, ARDUINO])
        self.assertEqual(ports[0].kind, "unknown")
        self.assertTrue(ports[0].likely_esp)
        self.assertTrue(ports[0].auto_reset)
        self.assertFalse(ports[1].likely_esp)
        self.assertEqual([p.device for p in esp_candidates(ports)], ["/dev/ttyUSB2"])
        self.assertEqual(select_port(ports).device, "/dev/ttyUSB2")

    def test_description_stands_in_for_a_missing_product(self):
        p = SimpleNamespace(device="COM5", vid=0x303A, pid=0x1001, serial_number="X", product=None,
                            location=None, description="USB JTAG/serial debug unit (COM5)")
        self.assertIn("USB JTAG/serial debug unit", list_ports([p])[0].label())

    def test_select_one_obvious_port(self):
        ports = list_ports([LEGACY, S3])
        self.assertEqual(select_port(ports).device, "/dev/ttyACM0")

    def test_two_candidates_is_ambiguous_not_first(self):
        ports = list_ports([S3, CP2102])
        self.assertIsNone(select_port(ports))
        self.assertEqual(select_port(ports, device="/dev/ttyUSB0").kind, "CP2102")
        self.assertEqual(select_port(ports, serial="7C:DF:A1:12:34:56").device, "/dev/ttyACM0")

    def test_a_serial_that_is_a_mac_names_the_chip_on_either_face(self):
        # --serial copied from `list` in one face finds the chip in the other:
        # the ROM's 1C:DB:D4:82:14:54 and the application's 1CDBD4821454 are
        # one chip. A serial that is not a MAC is matched as written.
        self.assertEqual(select_port(list_ports([S3_ROM, CP2102]), serial="1CDBD4821454").kind, "usb_serial_jtag")
        self.assertEqual(select_port(list_ports([COMPOSITE, CP2102]), serial="1C:DB:D4:82:14:54").kind, "retimesh_composite")
        self.assertEqual(select_port(list_ports([COMPOSITE, FT2232]), serial="AB12").kind, "unknown")
        self.assertIsNone(select_port(list_ports([COMPOSITE, CP2102]), serial="AA:BB:CC:DD:EE:FF"))

    def test_cp2102_serial_0001_is_never_a_handle(self):
        # Every CP2102 says 0001, so two of them cannot be told apart by it.
        ports = list_ports([CP2102, CP2102_B])
        self.assertIsNone(select_port(ports, serial="0001"))
        self.assertIn("at 1-4", ports[0].label())
        self.assertNotIn("serial 0001", ports[0].label())

    def test_explicit_device_that_is_absent(self):
        self.assertIsNone(select_port(list_ports([S3]), device="/dev/ttyUSB9"))

    def test_a_symlink_path_names_the_same_port(self):
        import os, tempfile
        with tempfile.TemporaryDirectory() as d:
            real = os.path.join(d, "ttyACM0"); open(real, "w").close()
            link = os.path.join(d, "by-id-link"); os.symlink(real, link)
            ports = list_ports([fake_comport(real, 0x303A, 0x1001, "X", "USB JTAG/serial debug unit")])
            self.assertIsNotNone(select_port(ports, device=link))
            self.assertTrue(device.same_device(link, real))


class ConsoleTest(unittest.TestCase):
    def test_version_reply_is_parsed_past_log_lines(self):
        con = Console(FakeSerial(), timeout=1.0)
        status, kv, data = con.command("VERSION")
        self.assertEqual(status, "OK")
        self.assertEqual(data[0]["firmware"], "RetiMesh Node")
        self.assertEqual(data[0]["board"], "LilyGO T3-S3")

    def test_a_reply_missing_a_line_is_asked_for_again(self):
        # The S3 loses the first line after open. The count on the OK line
        # shows the loss, and the command goes out a second time.
        ser = FakeSerial(drop_first=True)
        status, kv, data = Console(ser, timeout=1.0).command("VERSION")
        self.assertEqual((status, data[0]["board"]), ("OK", "LilyGO T3-S3"))
        self.assertEqual(ser.sent.count("VERSION\n"), 2)
        self.assertNotIn("lines", kv)

    def test_a_command_glued_onto_stale_bytes_is_asked_again(self):
        ser = FakeSerial(stale_prefix=True)
        status, _, data = Console(ser, timeout=1.0).command("VERSION")
        self.assertEqual((status, data[0]["board"]), ("OK", "LilyGO T3-S3"))
        self.assertEqual(ser.sent.count("VERSION\n"), 2)

    def test_a_reply_that_stays_short_is_reported_not_trusted(self):
        ser = FakeSerial(drop_always=True)
        status, _, data = Console(ser, timeout=1.0).command("VERSION")
        self.assertEqual((status, data), ("SHORT", []))
        self.assertEqual(ser.sent.count("VERSION\n"), 2)     # once more, not forever

    def test_a_node_that_does_not_count_is_taken_at_its_word(self):
        ser = FakeSerial(counts=False)
        status, _, data = Console(ser, timeout=1.0).command("VERSION")
        self.assertEqual((status, len(data)), ("OK", 1))
        self.assertEqual(ser.sent.count("VERSION\n"), 1)

    def test_error_replies_carry_code_and_text(self):
        con = Console(FakeSerial(), timeout=1.0)
        status, kv, _ = con.command("BOOTLOADER")
        self.assertEqual((status, kv["code"]), ("ERR", 400))
        self.assertIn("CONFIRM", kv["text"])

    def test_silence_times_out_rather_than_hanging(self):
        # The clock is injected, not patched into the stdlib: an earlier
        # version replaced time.monotonic for the whole interpreter.
        clock = Clock()
        ser = FakeSerial(silent=True)
        ser.read = lambda n=1: (clock.sleep(0.2), b"")[1]
        con = Console(ser, timeout=1.0, clock=clock.now)
        self.assertEqual(con.command("VERSION")[0], "TIMEOUT")

    def test_version_on_an_open_session(self):
        con = Console(FakeSerial(board="Heltec V3"), timeout=1.0, device="/dev/ttyUSB0")
        info = device.probe_console("/dev/ttyUSB0", console=con)
        self.assertEqual((info.board, info.via), ("Heltec V3", "console:/dev/ttyUSB0"))

    def test_a_port_that_dies_mid_read_is_no_answer_not_a_traceback(self):
        class Dying(FakeSerial):
            def read(self, n=1):
                raise OSError("device disconnected")
        self.assertIsNone(device.probe_console("/dev/ttyACM0", console=Console(Dying(), 1.0)))

    def test_node_url_normalises_what_people_type(self):
        self.assertEqual(node_url("10.42.0.1"), "http://10.42.0.1")
        self.assertEqual(node_url("http://10.42.0.1/"), "http://10.42.0.1")
        self.assertEqual(node_url(" https://retimesh.local "), "https://retimesh.local")
        self.assertEqual(node_url("httpfoo.local"), "http://httpfoo.local")

    def test_kv_parsing_handles_quotes(self):
        self.assertEqual(parse_kv('a=1 b="two words" c=x'), {"a": "1", "b": "two words", "c": "x"})


class HandOffTest(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.log = []

    def run_handoff(self, ports_seq, probe, request_http=None, port=None, node_url=None,
                    serial_factory=None, opened=None, rom=None, touch=None):
        """ports_seq: list of port lists returned on successive scans (last repeats).
        `probe` answers the initial VERSION and the post-request "still
        answering?" check; it takes (device, timeout=..., console=...)."""
        calls = {"n": 0}
        def ports_fn():
            i = min(calls["n"], len(ports_seq) - 1); calls["n"] += 1
            return list_ports(ports_seq[i])
        def open_console(dev, timeout=2.0):
            if opened is not None:
                opened.append(dev)
            return Console((serial_factory or FakeSerial)(), 1.0, device=dev)
        # `rom` stands in for esptool's sync: whether a downloader answers on
        # the port — a bool, or a callable for a test where that changes.
        if rom is None:
            rom = True
        probe_rom = rom if callable(rom) else (lambda dev: rom)
        return hand_off_to_bootloader(port=port, node_url_text=node_url, log=self.log.append,
                                      ports_fn=ports_fn, probe=probe, open_console=open_console,
                                      request_http=request_http or (lambda *a, **k: (False, "no HTTP answer", 0)),
                                      probe_rom=probe_rom, sleep=self.clock.sleep, clock=self.clock.now,
                                      reappear_timeout=8.0, **({"touch": touch} if touch else {}))

    @staticmethod
    def answers(info):
        """A probe that names the node before the request and is silent after
        it — i.e. a node that really did reset."""
        state = {"asked": 0}
        def probe(dev, timeout=2.0, console=None):
            state["asked"] += 1
            return info if console is not None else None
        return probe

    def test_console_path_on_an_s3(self):
        # The S3's port has to vanish (the reset) and come back (the ROM
        # downloader) before the downloader is declared present.
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        opened = []
        r = self.run_handoff([[S3], [S3], [], [], [S3]], probe=self.answers(info), opened=opened)
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port), ("console", "/dev/ttyACM0"))
        self.assertEqual(opened, ["/dev/ttyACM0"])            # one session for VERSION and the request
        # esptool runs its own reset at connect on any port that can drive one,
        # even into a downloader that is already up (see HandOff.esptool_before).
        self.assertEqual(r.esptool_before, "default_reset")
        self.assertTrue(any("accepted BOOTLOADER" in l for l in self.log))

    def test_an_s3_that_keeps_answering_did_not_reset(self):
        # The application acknowledged and then kept running (a crash before
        # the shutdown handler, say): its port never drops and its console
        # still answers. An earlier version saw the port present at ack+0.3 s,
        # called that the downloader, and told esptool no_reset — switching
        # off the one recovery that would have worked.
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: info)
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")
        self.assertEqual(r.esptool_before, "default_reset")
        self.assertIn("still answers", r.message)

    def test_an_s3_whose_reset_gap_was_missed_is_judged_by_its_silence(self):
        # On the bench the S3 re-enumerates in under the time it takes to look:
        # the port was never seen absent, yet the node did reset. A silent
        # console is a downloader, and esptool must not be told to reset it.
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        r = self.run_handoff([[S3]], probe=self.answers(info))
        self.assertTrue(r.entered)
        # esptool runs its own reset at connect on any port that can drive one,
        # even into a downloader that is already up (see HandOff.esptool_before).
        self.assertEqual(r.esptool_before, "default_reset")
        self.assertIn("confirmed by esptool", r.message)

    def test_a_silent_port_where_a_downloader_already_answers(self):
        # Left in the ROM downloader by an earlier attempt: no console, but
        # esptool syncs. esptool still runs its own reset at connect — on the
        # serial-JTAG unit that is what keeps the ROM there through esptool's
        # closing hard reset (HandOff.esptool_before).
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: None, rom=True)
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.esptool_before), ("downloader", "default_reset"))

    def test_a_silent_port_with_no_downloader_is_left_to_esptool(self):
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: None, rom=False)
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")

    def test_a_port_that_comes_back_without_a_downloader_is_not_entered(self):
        # The node reset, its port returned — but into the application (the
        # download-boot bit did not take). esptool must be allowed its reset.
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        r = self.run_handoff([[S3], [], [S3]], probe=self.answers(info), rom=False)
        self.assertFalse(r.entered)
        self.assertIn("no ROM downloader answers", r.message)

    def test_the_composite_device_hands_off_to_its_serial_jtag_downloader(self):
        # The console accepts; the composite port goes; the ROM comes up on
        # the serial-JTAG unit of the same chip, where esptool is pointed —
        # with its own reset at connect, which that downloader needs.
        r = self.run_handoff([[COMPOSITE], [COMPOSITE], [], [S3_ROM]], probe=self.answers(COMPOSITE_INFO))
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port, r.esptool_before), ("console", "/dev/ttyACM5", "default_reset"))
        # The chip's identity travels with the answer: the downloader's port
        # name carries none, and it is what the callers hold afterwards.
        self.assertEqual(r.node_id, "1CDBD4821454")

    def test_a_touch_that_cannot_open_the_port_is_reported_not_waited_for(self):
        # No console (switched off, or the port is held by something else)
        # and the touch cannot open the port either: that is an answer now,
        # not three minutes of waiting for a downloader nobody asked for.
        def touch(dev):
            raise OSError("Device or resource busy")
        r = self.run_handoff([[COMPOSITE]], probe=lambda dev, timeout=2.0, console=None: None, touch=touch)
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "none")
        self.assertIn("could not open /dev/ttyACM5", r.message)
        self.assertIn("hold BOOT", r.message)
        self.assertLess(self.clock.t, 5.0)

    def test_a_nameless_composite_device_is_followed_only_to_a_new_downloader(self):
        # No usable serial, so the chip cannot be known by MAC; a serial-JTAG
        # unit that was already on the bus is another chip's and is left
        # alone, and the one that appears after the request is taken.
        nameless, other = COMPOSITE_NAMELESS, OTHER_S3_ROM
        r = self.run_handoff([[nameless, other], [nameless, other], [other], [other], [other, S3_ROM]],
                             probe=self.answers(COMPOSITE_INFO), port="/dev/ttyACM5")
        self.assertTrue(r.entered)
        self.assertEqual(r.port, "/dev/ttyACM5")     # S3_ROM's device, the newcomer — not /dev/ttyACM9

    def test_two_nameless_composite_devices_do_not_count_each_other_as_gone(self):
        # With the MAC unknown the port is followed by path: the neighbour
        # staying on the bus does not make ours look gone, nor the reverse.
        ours = COMPOSITE_NAMELESS
        neighbour = fake_comport("/dev/ttyACM6", 0x1209, 0x0001, None, "RetiMesh Node", "1-12")
        # ours never leaves: the node did not reset, and the console still answers
        r = self.run_handoff([[ours, neighbour]] * 3, probe=lambda dev, timeout=2.0, console=None: COMPOSITE_INFO,
                             port="/dev/ttyACM5")
        self.assertFalse(r.entered)
        self.assertIn("still answers", r.message)

    def test_a_composite_device_whose_console_cannot_enter_is_touched(self):
        touched = []
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM5")
        r = hand_off_to_bootloader(port=None, log=self.log.append,
                                   ports_fn=iter([list_ports([COMPOSITE])] * 2 + [list_ports([S3_ROM])] * 9).__next__,
                                   probe=self.answers(info),
                                   open_console=lambda dev, timeout=2.0: Console(FakeSerial(software_entry=False), 1.0, device=dev),
                                   request_http=lambda *a, **k: (False, "", 0), probe_rom=lambda dev: True,
                                   sleep=self.clock.sleep, clock=self.clock.now, touch=touched.append)
        self.assertEqual(touched, ["/dev/ttyACM5"])
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port), ("touch", "/dev/ttyACM5"))

    def test_a_gone_composite_port_is_followed_to_its_downloader(self):
        # The by-id name of the composite port carries the MAC; the ROM is on
        # the serial-JTAG unit with the same MAC, so a hand-off asked for the
        # absent port lands on the downloader instead of giving up.
        by_id = "/dev/serial/by-id/usb-RetiMesh_RetiMesh_Node_1CDBD4821454-if00"
        self.assertEqual(device.node_id_from_path(by_id), "1CDBD4821454")
        self.assertEqual(device.node_id_from_path("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:82:14:54-if00"), "1CDBD4821454")
        r = self.run_handoff([[S3_ROM]], probe=lambda dev, timeout=2.0, console=None: None, port=by_id, rom=True)
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port, r.esptool_before), ("downloader", "/dev/ttyACM5", "default_reset"))

    def test_a_downloader_already_up_on_a_serial_jtag_unit_still_gets_esptool_reset(self):
        r = self.run_handoff([[S3_ROM]], probe=lambda dev, timeout=2.0, console=None: None, rom=True)
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.esptool_before), ("downloader", "default_reset"))

    def test_a_bridge_that_keeps_answering_did_not_reset(self):
        # On a CP2102 the port is the bridge's and never moves; the proof of a
        # reset is the console falling silent.
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "Heltec V3", "console:/dev/ttyUSB0")
        r = self.run_handoff([[CP2102]], probe=lambda dev, timeout=2.0, console=None: info)
        self.assertFalse(r.entered)
        self.assertIn("still answers", r.message)

    def test_a_bridge_that_falls_silent_is_in_the_downloader(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "Heltec V3", "console:/dev/ttyUSB0")
        r = self.run_handoff([[CP2102]], probe=self.answers(info))
        self.assertTrue(r.entered)
        self.assertEqual(r.port, "/dev/ttyUSB0")

    def test_classic_esp32_is_left_to_esptool(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T-Beam", "console:/dev/ttyUSB0")
        r = self.run_handoff([[CP2102]], probe=self.answers(info),
                             serial_factory=lambda: FakeSerial(software_entry=False))
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")
        self.assertEqual(r.port, "/dev/ttyUSB0")

    def test_no_console_falls_back_to_http(self):
        # No downloader before the request (or the console step would have
        # taken the port as already there), one after it.
        asked = []
        r = self.run_handoff([[S3], [S3], [], [S3]], probe=lambda dev, timeout=2.0, console=None: None,
                             node_url="http://10.42.0.1", request_http=lambda url, auth: (True, "accepted", 600),
                             rom=lambda dev: (asked.append(dev), len(asked) > 1)[1])
        self.assertTrue(r.entered)
        self.assertEqual(r.method, "http")
        self.assertTrue(any("HTTP bootloader request: accepted" in l for l in self.log))

    def test_http_refused_leaves_reset_to_esptool(self):
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: None, node_url="http://10.42.0.1",
                             request_http=lambda url, auth: (False, "HTTP 403: only from a directly attached link", 0),
                             rom=False)
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")

    def test_port_that_never_returns_is_a_bounded_failure(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        r = self.run_handoff([[S3], []], probe=self.answers(info))
        self.assertFalse(r.entered)
        self.assertIn("did not reappear", r.message)
        self.assertIn("hold BOOT", r.message)
        self.assertLess(self.clock.t, 15.0)

    def test_several_ports_without_a_choice_is_refused_with_names(self):
        r = self.run_handoff([[S3, CP2102]], probe=lambda dev, timeout=2.0, console=None: None, rom=False)
        self.assertFalse(r.entered)
        self.assertIn("several ESP32 ports", r.message)
        self.assertIn("/dev/ttyUSB0", r.message)
        self.assertIn("--port", r.message)

    def test_explicit_absent_port(self):
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: None, port="/dev/ttyUSB7", rom=False)
        self.assertIn("not present", r.message)

    def test_nothing_at_all(self):
        r = self.run_handoff([[LEGACY]], probe=lambda dev, timeout=2.0, console=None: None, rom=False)
        self.assertEqual(r.method, "none")
        self.assertIn("no ESP32 serial port", r.message)


PPPD_ARGV = ["pppd", "/dev/ttyUSB0", "115200", "noauth", "local", "nodetach", "nocrtscts",
             "lcp-echo-interval", "5", "lcp-echo-failure", "4", "10.65.84.2:10.65.84.1"]
PPP_ROUTE = "10.65.84.1 dev ppp0 proto kernel scope link src 10.65.84.2"


class PppTest(unittest.TestCase):
    def test_links_come_from_the_routing_table(self):
        # A host route through a pppN device is a PPP link; the node is the
        # far end and our own address the source. Everything else on the
        # table — the LAN, the default route, the USB link — is not one.
        routes = ["default via 192.168.1.1 dev wlan0 proto dhcp metric 600",
                  "10.64.84.0/24 dev enx02dbd4821455 proto kernel scope link src 10.64.84.2",
                  PPP_ROUTE,
                  "192.168.1.0/24 dev wlan0 proto kernel scope link src 192.168.1.10 metric 600"]
        links = device.ppp_links(routes)
        self.assertEqual([(l.ifname, l.node_ip, l.host_ip) for l in links], [("ppp0", "10.65.84.1", "10.65.84.2")])
        self.assertEqual(links[0].url, "http://10.65.84.1")
        self.assertEqual(device.ppp_links([]), [])

    def test_pppd_is_found_by_the_port_on_its_command_line(self):
        # Root's process, but its command line is everyone's to read; the
        # port is the /dev argument and the speed the bare number. A pppd
        # on another port, and a process that merely mentions pppd, are not
        # holders of this one. (A by-path name and the tty it points at are
        # one port: same_device, tested above.)
        procs = [(1, ["/sbin/init"]), (4242, ["/usr/sbin/pppd", *PPPD_ARGV[1:]]),
                 (4300, ["pppd", "/dev/ttyUSB1", "921600", "noauth"]), (5000, ["python3", "pppd", "notes.txt"])]
        held = device.find_pppd("/dev/ttyUSB0", cmdlines=procs)
        self.assertEqual([(p.pid, p.port, p.baud) for p in held], [(4242, "/dev/ttyUSB0", 115200)])
        self.assertEqual([p.pid for p in device.find_pppd(cmdlines=procs)], [4242, 4300])
        self.assertEqual(device.find_pppd("/dev/ttyACM0", cmdlines=procs), [])

    def test_the_pppd_command_is_built_from_what_the_node_asks_for(self):
        # The console's LINKS line for ppp0 carries the addresses the node
        # will ask for in IPCP and the port's speed; pppd is told the same,
        # host first in its local:remote pair.
        data = [{"link": "wifi-ap", "type": "wifi_ap", "hardware": "yes", "firmware": "yes", "enabled": "yes"},
                {"link": "ppp0", "type": "ppp_uart", "hardware": "yes", "firmware": "yes", "enabled": "no",
                 "baud": "115200", "asks": "10.65.84.1", "peer": "10.65.84.2"}]
        node, host, baud, enabled = device.ppp_addresses(data)
        self.assertEqual((node, host, baud, enabled), ("10.65.84.1", "10.65.84.2", 115200, False))
        self.assertEqual(device.pppd_command("/dev/ttyUSB0", node, host, baud), PPPD_ARGV)
        # The unprivileged form takes the same options from the peers file.
        self.assertEqual(device.pppd_command("/dev/ttyUSB0", node, host, baud, peers=True),
                         ["pppd", "/dev/ttyUSB0", str(baud), "call", "retimesh", f"{host}:{node}"])
        self.assertEqual(device.pppd_peers_file(), "noauth\nlocal\nnodetach\nnocrtscts\nlcp-echo-interval 5\nlcp-echo-failure 4\n")
        self.assertIn("10.65.84.2:10.65.84.1", device.shell_words(PPPD_ARGV))
        # A board with no PPP names no addresses.
        no_ppp = [{"link": "ppp0", "type": "ppp_uart", "hardware": "no", "firmware": "no", "enabled": "no"}]
        self.assertIsNone(device.ppp_addresses(no_ppp))


class PppHandOffTest(unittest.TestCase):
    """The hand-off while pppd holds the port: the console is not opened,
    the node is asked over ppp0, and esptool waits for the port."""

    def setUp(self):
        self.clock = Clock()
        self.log = []
        self.opened = []

    def run_handoff(self, request_http, pppd_exits_after=None, links=(PPP_ROUTE,), rom=True):
        # pppd holds the port until `pppd_exits_after` seconds have passed on
        # the injected clock; never, when None.
        started = self.clock.now()
        def pppd_fn(port=None):
            gone = pppd_exits_after is not None and self.clock.now() - started >= pppd_exits_after
            return [] if gone else device.find_pppd(port, cmdlines=[(4242, PPPD_ARGV)])
        return hand_off_to_bootloader(port="/dev/ttyUSB0", log=self.log.append,
                                      ports_fn=lambda: list_ports([CP2102]),
                                      probe=lambda dev, timeout=2.0, console=None: None,
                                      open_console=lambda dev, timeout=2.0: (self.opened.append(dev), Console(FakeSerial(), 1.0, device=dev))[1],
                                      request_http=request_http, probe_rom=lambda dev: rom,
                                      sleep=self.clock.sleep, clock=self.clock.now,
                                      pppd_fn=pppd_fn, ppp_links_fn=lambda: device.ppp_links(links))

    def test_an_s3_behind_a_bridge_is_asked_over_ppp_and_waited_for(self):
        asked = []
        r = self.run_handoff(lambda url, auth: (asked.append(url), (True, "accepted (software_api, 600 ms)", 600))[1],
                             pppd_exits_after=12.0)
        self.assertEqual(asked, ["http://10.65.84.1"])           # the far end of the link, not a guess
        self.assertEqual(self.opened, [])                        # the port was never opened under pppd
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port, r.esptool_before), ("http", "/dev/ttyUSB0", "default_reset"))
        self.assertTrue(any("let go" in l for l in self.log))

    def test_a_classic_esp32_says_how_to_stop_pppd(self):
        # 501: esptool has to reset it, and cannot while pppd has the port.
        # Nothing here kills a root process; the message names the command.
        r = self.run_handoff(lambda url, auth: (False, "HTTP 501: this chip cannot enter its downloader from software", 0))
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "none")
        self.assertIn("sudo kill 4242", r.message)
        self.assertIn("501", r.message)

    def test_a_pppd_that_never_lets_go_is_a_bounded_failure(self):
        r = self.run_handoff(lambda url, auth: (True, "accepted", 600), pppd_exits_after=None)
        self.assertFalse(r.entered)
        self.assertIn("still holds /dev/ttyUSB0", r.message)
        self.assertIn("sudo kill 4242", r.message)
        self.assertLess(self.clock.t, 60.0)

    def test_pppd_without_a_link_up_is_reported_not_guessed(self):
        # pppd is dialling (or stuck): no route yet, so no address to ask at.
        r = self.run_handoff(lambda url, auth: (True, "accepted", 600), links=())
        self.assertFalse(r.entered)
        self.assertIn("no ppp interface", r.message)
        self.assertEqual(self.opened, [])


class HttpTest(unittest.TestCase):
    def test_probe_recognises_a_node(self):
        fetch = lambda url, body=None, auth=None, timeout=3.0: (200, {"firmware": "RetiMesh Node", "version": "v1", "power": {"board": "Heltec"}})
        info = probe_http("http://10.42.0.1/", fetch=fetch)
        self.assertEqual((info.version, info.board), ("v1", "Heltec"))

    def test_the_board_is_read_from_either_shape(self):
        # It moved to the top of the document; a node running older firmware
        # still answers with it under "power", and both must name the board.
        def fetch_for(doc):
            return lambda url, body=None, auth=None, timeout=3.0: (200, doc)
        new = {"firmware": "RetiMesh Node", "version": "v1", "board": "LilyGO T3-S3",
               "power": {"profile": "performance"}}
        old = {"firmware": "RetiMesh Node", "version": "v1", "power": {"board": "LilyGO T3-S3"}}
        for doc in (new, old):
            self.assertEqual(probe_http("http://10.42.0.1", fetch=fetch_for(doc)).board, "LilyGO T3-S3")
        # Neither shape: a question mark, never a traceback.
        bare = {"firmware": "RetiMesh Node", "version": "v1"}
        self.assertEqual(probe_http("http://10.42.0.1", fetch=fetch_for(bare)).board, "?")

    def test_probe_rejects_other_devices(self):
        fetch = lambda url, body=None, auth=None, timeout=3.0: (200, {"firmware": "Something Else"})
        self.assertIsNone(probe_http("http://10.42.0.1", fetch=fetch))

    def test_a_body_that_is_not_an_object_is_not_a_node(self):
        # A captive page or another device answering the path with null, a
        # list or a string must read as "no node", not as a traceback.
        for body in (None, [], "text", 42):
            fetch = lambda url, b=None, auth=None, timeout=3.0, body=body: (200, body)
            self.assertIsNone(probe_http("http://10.42.0.1", fetch=fetch))
            self.assertFalse(request_bootloader_http("http://x", fetch=fetch)[0])

    def test_bootloader_request_outcomes(self):
        ok = lambda url, body=None, auth=None, timeout=3.0: (202, {"method": "software_api", "delay_ms": 600})
        self.assertEqual(request_bootloader_http("http://x", fetch=ok)[0], True)
        self.assertEqual(request_bootloader_http("http://x", fetch=ok)[2], 600)
        refused = lambda url, body=None, auth=None, timeout=3.0: (403, {"error": "switched off"})
        self.assertEqual(request_bootloader_http("http://x", fetch=refused), (False, "HTTP 403: switched off", 0))
        dead = lambda url, body=None, auth=None, timeout=3.0: (0, {})
        self.assertEqual(request_bootloader_http("http://x", fetch=dead)[1], "no HTTP answer")


class EsptoolTest(unittest.TestCase):
    def test_the_argument_builder_runs_at_all(self):
        # The helper it needs once lived in another module; this call raised
        # NameError in every real invocation while the tests, which inject the
        # downloader check, stayed green.
        args = esptool_args("esp32s3", "/dev/ttyACM0", "no_reset", "hard_reset", "write_flash", "0x0", "fw.bin", baud=921600)
        self.assertEqual(args[:4], ["--chip", "esp32s3", "--port", "/dev/ttyACM0"])
        self.assertIn("--baud", args)
        self.assertIn("fw.bin", args)
        # Without a chip the option is left out, so esptool detects it.
        self.assertEqual(esptool_args(None, "/dev/x", "no_reset", "no_reset", "chip_id")[:2], ["--port", "/dev/x"])

    def test_a_handoff_over_http_with_no_port_is_not_entered(self):
        # Requested is not the same as known-to-be-up on a port esptool can
        # use; the hook would otherwise tell esptool no_reset for a port
        # PlatformIO finds a moment later, unchecked.
        clock = Clock()
        r = hand_off_to_bootloader(port=None, node_url_text="http://10.42.0.1", ports_fn=lambda: [],
                                   probe=lambda d, timeout=2.0, console=None: None,
                                   request_http=lambda url, auth: (True, "accepted", 600),
                                   probe_rom=lambda d: True, sleep=clock.sleep, clock=clock.now, reappear_timeout=2.0)
        self.assertFalse(r.entered)
        self.assertEqual(r.esptool_before, "default_reset")
        self.assertIsNone(r.port)

    def test_an_absent_named_port_still_tries_http(self):
        clock = Clock()
        asked = []
        r = hand_off_to_bootloader(port="/dev/ttyACM9", node_url_text="http://10.42.0.1",
                                   ports_fn=lambda: list_ports([S3]),
                                   probe=lambda d, timeout=2.0, console=None: None,
                                   request_http=lambda url, auth: (asked.append(url), (True, "accepted", 600))[1],
                                   probe_rom=lambda d: True, sleep=clock.sleep, clock=clock.now, reappear_timeout=2.0)
        self.assertEqual(asked, ["http://10.42.0.1"])
        self.assertTrue(r.entered)


class WaitForApplicationTest(unittest.TestCase):
    def test_returns_when_version_answers(self):
        # Asked, not listened for: the node is polled with VERSION until it
        # answers, whether or not anyone saw its boot banner.
        clock = Clock()
        answers = iter([None, None, device.NodeInfo("RetiMesh Node", "v2", "T3-S3", "console:/dev/ttyACM0")])
        asked = []
        info = wait_for_application("/dev/ttyACM0", 10.0,
                                    probe=lambda d, timeout=2.0, console=None: (asked.append(d), next(answers))[1],
                                    ports_fn=lambda: list_ports([S3]), sleep=clock.sleep, clock=clock.now)
        self.assertEqual(info.version, "v2")
        self.assertEqual(asked, ["/dev/ttyACM0"] * 3)

    def test_gives_up_on_time(self):
        clock = Clock()
        info = wait_for_application("/dev/ttyACM0", 5.0, probe=lambda d, timeout=2.0, console=None: None,
                                    ports_fn=lambda: list_ports([S3]), sleep=clock.sleep, clock=clock.now)
        self.assertIsNone(info)
        self.assertGreaterEqual(clock.t, 5.0)

    def test_with_no_hint_every_candidate_is_asked(self):
        # A bench with a node and an RNode: both look like ESP32 ports. Neither
        # is "the" port, so both are asked and the one that answers wins.
        clock = Clock()
        rnode = fake_comport("/dev/ttyUSB3", 0x10C4, 0xEA60, "0001", "CP2102N")
        info = wait_for_application(None, 10.0,
                                    probe=lambda d, timeout=2.0, console=None:
                                        device.NodeInfo("RetiMesh Node", "v2", "T3-S3", f"console:{d}") if d == "/dev/ttyACM0" else None,
                                    ports_fn=lambda: list_ports([rnode, S3]), sleep=clock.sleep, clock=clock.now)
        self.assertEqual(info.via, "console:/dev/ttyACM0")

    def test_only_the_flashed_chip_is_asked_when_its_name_is_known(self):
        # The downloader was on the serial-JTAG unit of one chip; a soak node
        # on the bench must not be taken for the application coming back.
        asked = []
        def probe(dev, timeout=2.0, console=None):
            asked.append(dev); return COMPOSITE_INFO if dev == "/dev/ttyACM5" else None
        clock = Clock()
        seq = iter([list_ports([CP2102]), list_ports([CP2102]), list_ports([CP2102, COMPOSITE])] + [list_ports([CP2102, COMPOSITE])] * 20)
        r = device.wait_for_application("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:82:14:54-if00", 30.0,
                                        probe=probe, ports_fn=lambda: next(seq), sleep=clock.sleep, clock=clock.now)
        self.assertEqual(r, COMPOSITE_INFO)
        self.assertNotIn("/dev/ttyUSB0", asked)

    def test_the_hand_offs_identity_names_the_chip_when_the_port_name_does_not(self):
        # What the CLI and the hook hold after a hand-off is the downloader's
        # pyserial name, which names no chip; the identity comes from the
        # HandOff, in either spelling of the MAC.
        asked = []
        def probe(dev, timeout=2.0, console=None):
            asked.append(dev); return COMPOSITE_INFO if dev == "/dev/ttyACM5" else None
        clock = Clock()
        seq = iter([list_ports([CP2102]), list_ports([CP2102, COMPOSITE])] + [list_ports([CP2102, COMPOSITE])] * 20)
        r = device.wait_for_application("/dev/ttyACM5", 30.0, probe=probe, ports_fn=lambda: next(seq),
                                        sleep=clock.sleep, clock=clock.now, node_id="1C:DB:D4:82:14:54")
        self.assertEqual(r, COMPOSITE_INFO)
        self.assertNotIn("/dev/ttyUSB0", asked)

    def test_a_nameless_port_is_still_asked_for_the_flashed_chip(self):
        # The application came back as a composite device without a usable
        # serial (routine on Windows); it is asked, while a port naming
        # another chip is not.
        asked = []
        def probe(dev, timeout=2.0, console=None):
            asked.append(dev); return COMPOSITE_INFO if dev == "/dev/ttyACM5" else None
        clock = Clock()
        r = device.wait_for_application("/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:82:14:54-if00", 30.0,
                                        probe=probe, ports_fn=lambda: list_ports([OTHER_S3_ROM, COMPOSITE_NAMELESS]),
                                        sleep=clock.sleep, clock=clock.now)
        self.assertEqual(r, COMPOSITE_INFO)
        self.assertNotIn("/dev/ttyACM9", asked)

    def test_an_esptool_that_cannot_run_is_not_an_answer_about_the_port(self):
        # A missing esptool returned the same "no downloader" as a silent
        # port, which sent a caller down the bridge-reset path and left a
        # node in its ROM. The verdict stays cautious; the log says why.
        said = []
        import subprocess
        real = subprocess.run
        try:
            subprocess.run = lambda *a, **k: SimpleNamespace(returncode=1, stdout="",
                                                             stderr="/usr/bin/python3: No module named esptool\n")
            self.assertFalse(device.downloader_present("/dev/ttyACM5", log=said.append))
        finally:
            subprocess.run = real
        self.assertTrue(any("not installed" in m for m in said), said)
        # And a port that simply is not there is named as that, not as esptool's fault.
        said.clear()
        try:
            subprocess.run = lambda *a, **k: (_ for _ in ()).throw(FileNotFoundError("no such file"))
            self.assertFalse(device.downloader_present("/dev/ttyACM5", log=said.append))
        finally:
            subprocess.run = real
        self.assertTrue(any("could not be run" in m for m in said), said)

    def test_the_post_flash_wait_is_one_rule_for_every_caller(self):
        # The CLI, the PlatformIO hook and the HIL script each used to carry a
        # number of their own (20, 90 and 40 s); two were under what a bench
        # hub takes to report the device arriving. One rule now, and the
        # native value is derived from the downloader wait so they cannot
        # drift apart.
        self.assertEqual(device._APPLICATION_BACK_NATIVE_S, device._COMPOSITE_DOWNLOADER_S)
        by_id = "/dev/serial/by-id/usb-RetiMesh_RetiMesh_Node_1CDBD4821454-if00"
        ports = lambda: list_ports([CP2102, COMPOSITE])
        # A chip named by its MAC, or by a by-id path, or sitting on one of
        # the two native-USB faces: the patient wait.
        self.assertEqual(device.application_wait_s("1C:DB:D4:82:14:54", None, ports), device._APPLICATION_BACK_NATIVE_S)
        self.assertEqual(device.application_wait_s(None, by_id, ports), device._APPLICATION_BACK_NATIVE_S)
        self.assertEqual(device.application_wait_s(None, "/dev/ttyACM5", ports), device._APPLICATION_BACK_NATIVE_S)
        # A bridge board's port never moves, so silence is a real failure.
        self.assertEqual(device.application_wait_s(None, "/dev/ttyUSB0", ports), device._APPLICATION_BACK_BRIDGE_S)

    def test_a_port_that_lingers_after_the_device_left_does_not_starve_the_chip(self):
        # The hub has not yet reported the ROM's port going, so the name the
        # caller holds still resolves — to a device that is not there. The
        # chip answers under its other face, and must be asked: an earlier
        # version took the named port as the only candidate and spent the
        # whole wait on a port nobody was behind.
        moved = fake_comport("/dev/ttyACM7", 0x1209, 0x0001, "1CDBD4821454", "RetiMesh Node", "1-11")
        asked = []
        def probe(dev, timeout=2.0, console=None):
            asked.append(dev)
            return COMPOSITE_INFO if dev == "/dev/ttyACM7" else None
        clock = Clock()
        r = device.wait_for_application("/dev/ttyACM5", 30.0, probe=probe,
                                        ports_fn=lambda: list_ports([S3_ROM, moved]),
                                        sleep=clock.sleep, clock=clock.now, node_id="1CDBD4821454")
        self.assertEqual(r, COMPOSITE_INFO)
        self.assertIn("/dev/ttyACM7", asked)
        self.assertLess(clock.t, 5.0)

    def test_the_application_may_come_back_under_another_name(self):
        # The downloader was ttyACM1 because ttyACM0 was briefly held; the
        # application re-enumerates as ttyACM0 again. The named port is absent,
        # the one ESP-looking port is accepted.
        clock = Clock()
        back = fake_comport("/dev/ttyACM0", 0x303A, 0x1001, "7C:DF:A1:12:34:56", "USB JTAG/serial debug unit")
        info = wait_for_application("/dev/ttyACM1", 10.0,
                                    probe=lambda d, timeout=2.0, console=None:
                                        device.NodeInfo("RetiMesh Node", "v2", "T3-S3", f"console:{d}"),
                                    ports_fn=lambda: list_ports([back]), sleep=clock.sleep, clock=clock.now)
        self.assertEqual(info.via, "console:/dev/ttyACM0")



class TargetNaming(unittest.TestCase):
    """Which transport a target names. The console answers on a cable and on
    a socket, and a client is handed one string for either."""

    def test_a_path_or_a_com_name_is_a_port(self):
        for t in ("/dev/ttyUSB0", "/dev/serial/by-id/usb-x", "COM7", "com3"):
            self.assertTrue(device.is_device_path(t), t)

    def test_everything_else_is_a_host(self):
        for t in ("192.168.1.50", "retimesh.local", "node", "::1", "[::1]:4243"):
            self.assertFalse(device.is_device_path(t), t)

    def test_a_port_that_does_not_exist_is_still_a_port(self):
        # Otherwise a typo'd path is quietly dialled as a hostname and the
        # error names DNS instead of the port the operator meant.
        self.assertTrue(device.is_device_path("/dev/ttyUSB9"))

    def test_a_bare_host_takes_the_default_port(self):
        self.assertEqual(("192.168.1.50", 4243), device.split_host_port("192.168.1.50"))
        self.assertEqual(("retimesh.local", 4243), device.split_host_port("retimesh.local"))

    def test_a_port_may_be_given(self):
        self.assertEqual(("192.168.1.50", 9999), device.split_host_port("192.168.1.50:9999"))
        self.assertEqual(("node", 22), device.split_host_port("node:22"))

    def test_ipv6_keeps_its_colons(self):
        # "::1" is an address, not a host called ":" on port 1; a port comes
        # with brackets, which is the only way to tell them apart.
        self.assertEqual(("::1", 4243), device.split_host_port("::1"))
        self.assertEqual(("fe80::1%eth0", 4243), device.split_host_port("fe80::1%eth0"))
        self.assertEqual(("::1", 4243), device.split_host_port("[::1]:4243"))
        self.assertEqual(("::1", 9999), device.split_host_port("[::1]:9999"))


class ConsoleKeepsWhatTheNodeSaid(unittest.TestCase):
    def test_the_raw_reply_lines_survive_parsing(self):
        # A client showing an operator the reply shows the node's words, not
        # a reconstruction of them from the parsed pairs — and the log line
        # the port carries is not one of them.
        con = Console(FakeSerial(board="Heltec V3"), timeout=1.0)
        status, _, data = con.command("VERSION")
        self.assertEqual("OK", status)
        self.assertEqual(1, len(data))
        self.assertTrue(all(l.startswith("RM ") for l in con.last_lines), con.last_lines)
        self.assertTrue(con.last_lines[0].startswith("RM VERSION "))
        self.assertTrue(con.last_lines[-1].startswith("RM OK VERSION"))
        self.assertIn("Heltec V3", con.last_lines[0])

    def test_the_lines_are_the_last_command_s_only(self):
        con = Console(FakeSerial(), timeout=1.0)
        con.command("VERSION")
        first = list(con.last_lines)
        con.command("VERSION")
        self.assertEqual(first, con.last_lines)      # not appended to

    def test_a_cable_session_does_not_authenticate(self):
        # The cable is trusted; only a socket session sends AUTH, and a
        # client that got this wrong would spend the node's failure budget.
        self.assertFalse(Console(FakeSerial(), timeout=1.0).networked)

if __name__ == "__main__":
    unittest.main()
