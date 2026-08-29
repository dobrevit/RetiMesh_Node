# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Host-side tests for retimesh_flash.device with no hardware: fake ports, a
fake serial transport that speaks the maintenance protocol, a fake HTTP.

Run: python -m unittest discover -s tools/retimesh-flash/tests
"""
import unittest
from types import SimpleNamespace

from retimesh_flash import device
from retimesh_flash.device import (Console, HandOff, Port, esp_candidates, hand_off_to_bootloader, list_ports,
                                   node_url, parse_kv, probe_http, request_bootloader_http, select_port,
                                   wait_for_application)


def fake_comport(dev, vid=None, pid=None, serial=None, product=None, location=None):
    return SimpleNamespace(device=dev, vid=vid, pid=pid, serial_number=serial, product=product, location=location)


S3 = fake_comport("/dev/ttyACM0", 0x303A, 0x1001, "7C:DF:A1:12:34:56", "USB JTAG/serial debug unit", "1-3")
CP2102 = fake_comport("/dev/ttyUSB0", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-4")
CP2102_B = fake_comport("/dev/ttyUSB1", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-5")
LEGACY = fake_comport("/dev/ttyS0")
ARDUINO = fake_comport("/dev/ttyACM1", 0x2341, 0x0043, "555", "Arduino Uno")
FT2232 = fake_comport("/dev/ttyUSB2", 0x0403, 0x6010, "AB12", "FT2232H Dual UART")   # a known vendor, an unlisted product


class FakeSerial:
    """Answers like the firmware's Maintenance.cpp, log lines included."""

    def __init__(self, board="LilyGO T3-S3", software_entry=True, silent=False):
        self.board = board
        self.software_entry = software_entry
        self.silent = silent
        self.out = bytearray()
        self.sent = []
        self.dtr = self.rts = None

    def write(self, data):
        self.sent.append(data.decode())
        if self.silent:
            return
        line = data.decode().strip().upper()
        reply = ["[I][main.cpp:200] heartbeat line that shares the port"]
        if line == "VERSION":
            reply += [f'RM VERSION firmware="RetiMesh Node" version=v0.2.0 board="{self.board}" idf=v4.4.7',
                      "RM OK VERSION"]
        elif line == "BOOTLOADER CONFIRM":
            if self.software_entry:
                reply += ["RM OK BOOTLOADER target=bootloader method=software_api delay_ms=300"]
            else:
                reply += ["RM ERR BOOTLOADER 501 this chip cannot enter its downloader from software"]
        elif line == "BOOTLOADER":
            reply += ["RM ERR BOOTLOADER 400 add CONFIRM: BOOTLOADER CONFIRM"]
        else:
            reply += [f"RM ERR {line.split()[0]} 404 unknown command, try HELP"]
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
                    serial_factory=None, opened=None, rom=None):
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
                                      reappear_timeout=8.0)

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
        self.assertEqual(r.esptool_before, "no_reset")
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
        self.assertEqual(r.esptool_before, "no_reset")
        self.assertIn("confirmed by esptool", r.message)

    def test_a_silent_port_where_a_downloader_already_answers(self):
        # Left in the ROM downloader by an earlier attempt: no console, but
        # esptool syncs. This must be no_reset — a DTR/RTS reset on top
        # re-enumerates the port under esptool's own open.
        r = self.run_handoff([[S3]], probe=lambda dev, timeout=2.0, console=None: None, rom=True)
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.esptool_before), ("downloader", "no_reset"))

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


class HttpTest(unittest.TestCase):
    def test_probe_recognises_a_node(self):
        fetch = lambda url, body=None, auth=None, timeout=3.0: (200, {"firmware": "RetiMesh Node", "version": "v1", "power": {"board": "Heltec"}})
        info = probe_http("http://10.42.0.1/", fetch=fetch)
        self.assertEqual((info.version, info.board), ("v1", "Heltec"))

    def test_probe_rejects_other_devices(self):
        fetch = lambda url, body=None, auth=None, timeout=3.0: (200, {"firmware": "Something Else"})
        self.assertIsNone(probe_http("http://10.42.0.1", fetch=fetch))

    def test_bootloader_request_outcomes(self):
        ok = lambda url, body=None, auth=None, timeout=3.0: (202, {"method": "software_api", "delay_ms": 600})
        self.assertEqual(request_bootloader_http("http://x", fetch=ok)[0], True)
        self.assertEqual(request_bootloader_http("http://x", fetch=ok)[2], 600)
        refused = lambda url, body=None, auth=None, timeout=3.0: (403, {"error": "switched off"})
        self.assertEqual(request_bootloader_http("http://x", fetch=refused), (False, "HTTP 403: switched off", 0))
        dead = lambda url, body=None, auth=None, timeout=3.0: (0, {})
        self.assertEqual(request_bootloader_http("http://x", fetch=dead)[1], "no HTTP answer")


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


if __name__ == "__main__":
    unittest.main()
