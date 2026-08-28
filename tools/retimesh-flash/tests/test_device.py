# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Host-side tests for retimesh_flash.device with no hardware: fake ports, a
fake serial transport that speaks the maintenance protocol, a fake HTTP.

Run: python -m unittest discover -s tools/retimesh-flash/tests
"""
import unittest
from types import SimpleNamespace

from retimesh_flash import device
from retimesh_flash.device import (Console, HandOff, Port, hand_off_to_bootloader, list_ports,
                                   parse_kv, probe_http, request_bootloader_http, select_port,
                                   wait_for_application)


def fake_comport(dev, vid=None, pid=None, serial=None, product=None, location=None):
    return SimpleNamespace(device=dev, vid=vid, pid=pid, serial_number=serial, product=product, location=location)


S3 = fake_comport("/dev/ttyACM0", 0x303A, 0x1001, "7C:DF:A1:12:34:56", "USB JTAG/serial debug unit", "1-3")
CP2102 = fake_comport("/dev/ttyUSB0", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-4")
CP2102_B = fake_comport("/dev/ttyUSB1", 0x10C4, 0xEA60, "0001", "CP2102N USB to UART Bridge Controller", "1-5")
LEGACY = fake_comport("/dev/ttyS0")
ARDUINO = fake_comport("/dev/ttyACM1", 0x2341, 0x0043, "555", "Arduino Uno")


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
        clock = Clock()
        real = device.time.monotonic
        device.time.monotonic = clock.now
        try:
            ser = FakeSerial(silent=True)
            ser.read = lambda n=1: (clock.sleep(0.2), b"")[1]
            con = Console(ser, timeout=1.0)
            self.assertEqual(con.command("VERSION")[0], "TIMEOUT")
        finally:
            device.time.monotonic = real

    def test_kv_parsing_handles_quotes(self):
        self.assertEqual(parse_kv('a=1 b="two words" c=x'), {"a": "1", "b": "two words", "c": "x"})


class HandOffTest(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.log = []

    def run_handoff(self, ports_seq, probe, request_http=None, port=None, node_url=None):
        """ports_seq: list of port lists returned on successive scans (last repeats)."""
        calls = {"n": 0}
        def ports_fn():
            i = min(calls["n"], len(ports_seq) - 1); calls["n"] += 1
            return list_ports(ports_seq[i])
        return hand_off_to_bootloader(port=port, node_url=node_url, log=self.log.append,
                                      ports_fn=ports_fn, probe=probe,
                                      request_http=request_http or (lambda *a, **k: (False, "no HTTP answer")),
                                      sleep=self.clock.sleep, clock=self.clock.now, reappear_timeout=8.0)

    def test_console_path_on_an_s3(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        opened = []
        real_open = Console.open
        Console.open = staticmethod(lambda dev, timeout=2.0: (opened.append(dev), Console(FakeSerial(), 1.0))[1])
        try:
            r = self.run_handoff([[S3]], probe=lambda dev: info)
        finally:
            Console.open = real_open
        self.assertTrue(r.entered)
        self.assertEqual((r.method, r.port), ("console", "/dev/ttyACM0"))
        self.assertEqual(opened, ["/dev/ttyACM0"])
        self.assertTrue(any("accepted BOOTLOADER" in l for l in self.log))

    def test_classic_esp32_is_left_to_esptool(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T-Beam", "console:/dev/ttyACM0")
        real_open = Console.open
        Console.open = staticmethod(lambda dev, timeout=2.0: Console(FakeSerial(software_entry=False), 1.0))
        try:
            r = self.run_handoff([[CP2102]], probe=lambda dev: info)
        finally:
            Console.open = real_open
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")
        self.assertEqual(r.port, "/dev/ttyUSB0")

    def test_no_console_falls_back_to_http(self):
        r = self.run_handoff([[S3]], probe=lambda dev: None, node_url="http://10.42.0.1",
                             request_http=lambda url, auth: (True, "accepted"))
        self.assertTrue(r.entered)
        self.assertEqual(r.method, "http")
        self.assertEqual(r.tried, ["console on /dev/ttyACM0", "http http://10.42.0.1"])

    def test_http_refused_leaves_reset_to_esptool(self):
        r = self.run_handoff([[S3]], probe=lambda dev: None, node_url="http://10.42.0.1",
                             request_http=lambda url, auth: (False, "HTTP 403: only from a directly attached link"))
        self.assertFalse(r.entered)
        self.assertEqual(r.method, "auto_reset_dtr_rts")

    def test_port_that_never_returns_is_a_bounded_failure(self):
        info = device.NodeInfo("RetiMesh Node", "v0.2.0", "LilyGO T3-S3", "console:/dev/ttyACM0")
        real_open = Console.open
        Console.open = staticmethod(lambda dev, timeout=2.0: Console(FakeSerial(), 1.0))
        try:
            r = self.run_handoff([[S3], []], probe=lambda dev: info)
        finally:
            Console.open = real_open
        self.assertFalse(r.entered)
        self.assertIn("did not reappear", r.message)
        self.assertIn("hold BOOT", r.message)
        self.assertLess(self.clock.t, 15.0)

    def test_several_ports_without_a_choice_is_refused_with_names(self):
        r = self.run_handoff([[S3, CP2102]], probe=lambda dev: None)
        self.assertFalse(r.entered)
        self.assertIn("several ESP32 ports", r.message)
        self.assertIn("/dev/ttyUSB0", r.message)

    def test_explicit_absent_port(self):
        r = self.run_handoff([[S3]], probe=lambda dev: None, port="/dev/ttyUSB7")
        self.assertIn("not present", r.message)

    def test_nothing_at_all(self):
        r = self.run_handoff([[LEGACY]], probe=lambda dev: None)
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
        refused = lambda url, body=None, auth=None, timeout=3.0: (403, {"error": "switched off"})
        self.assertEqual(request_bootloader_http("http://x", fetch=refused), (False, "HTTP 403: switched off"))
        dead = lambda url, body=None, auth=None, timeout=3.0: (0, {})
        self.assertEqual(request_bootloader_http("http://x", fetch=dead)[1], "no HTTP answer")


class WaitForApplicationTest(unittest.TestCase):
    def test_returns_when_version_answers(self):
        clock = Clock()
        answers = iter([None, None, device.NodeInfo("RetiMesh Node", "v2", "T3-S3", "console:/dev/ttyACM0")])
        info = wait_for_application("/dev/ttyACM0", 10.0, probe=lambda d: next(answers),
                                    ports_fn=lambda: list_ports([S3]), sleep=clock.sleep, clock=clock.now)
        self.assertEqual(info.version, "v2")

    def test_gives_up_on_time(self):
        clock = Clock()
        info = wait_for_application("/dev/ttyACM0", 5.0, probe=lambda d: None,
                                    ports_fn=lambda: list_ports([S3]), sleep=clock.sleep, clock=clock.now)
        self.assertIsNone(info)
        self.assertGreaterEqual(clock.t, 5.0)


if __name__ == "__main__":
    unittest.main()
