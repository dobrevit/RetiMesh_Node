# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""
device.py — finding a RetiMesh node from the host, and getting it into its
bootloader.

Two views of the same node:

  * a serial port: the S3's USB-Serial/JTAG (303a:1001) on native-USB boards,
    a CP2102 (10c4:ea60) or CH9102 (1a86:55d4) on the bridged ones. The
    maintenance console (src/MaintenanceProtocol.h) answers on it, which is
    how a port is told apart from any other ESP32 on the bench: every reply
    line starts with "RM ", and VERSION names the firmware and board.

  * an HTTP endpoint: http://10.42.0.1 over the access point, 10.64.<n>.1 over
    USB networking, or wherever mDNS says. GET /api/status carries "firmware":
    "RetiMesh Node"; POST /api/system/bootloader is the other way in.

Ports are never assumed: /dev/ttyACM0 is whoever plugged in last. A node is
identified by what it says, and where several answer the caller picks by
serial number or path. CP2102 bridges all report serial "0001", so on those
the path is the only stable handle there is.

Every wait is bounded and every failure returns a message rather than raising,
because the PlatformIO hook and the CLI both want to explain, not crash.
"""
from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

# USB identities. The S3's fixed USB-Serial/JTAG unit is the same device in
# the application and in the ROM downloader; the RetiMesh composite device
# (src/UsbDescriptorPlan.h) will be a second pair, listed when it ships.
ESP_USB_SERIAL_JTAG = (0x303A, 0x1001)
BRIDGES = {(0x10C4, 0xEA60): "CP2102", (0x1A86, 0x55D4): "CH9102", (0x1A86, 0x7523): "CH340",
           (0x0403, 0x6001): "FT232", (0x0403, 0x6015): "FT231X"}
RETIMESH_PRODUCT = "RetiMesh Node"

CONSOLE_BAUD = 115200
REPLY_PREFIX = "RM "

Log = Callable[[str], None]


def _quiet(_: str) -> None:
    pass


# ---------------------------------------------------------------------------
# Serial ports
# ---------------------------------------------------------------------------
@dataclass
class Port:
    device: str                       # /dev/ttyACM0, COM5
    vid: Optional[int] = None
    pid: Optional[int] = None
    serial: Optional[str] = None
    product: Optional[str] = None
    location: Optional[str] = None    # USB topology path, stable per socket

    @property
    def kind(self) -> str:
        if (self.vid, self.pid) == ESP_USB_SERIAL_JTAG:
            return "usb_serial_jtag"
        return BRIDGES.get((self.vid, self.pid), "unknown" if self.vid is not None else "legacy")

    @property
    def auto_reset(self) -> bool:
        """Whether esptool's DTR/RTS reset is expected to work on this port."""
        return self.kind in ("usb_serial_jtag", *BRIDGES.values())

    def label(self) -> str:
        bits = [self.device, self.kind]
        if self.serial and self.serial != "0001":
            bits.append(f"serial {self.serial}")
        elif self.location:
            bits.append(f"at {self.location}")
        if self.product:
            bits.append(self.product)
        return " — ".join(bits)


def list_ports(comports: Optional[Iterable] = None) -> list[Port]:
    """USB serial ports, ESP-looking ones first. `comports` is injectable for
    tests; by default pyserial's."""
    if comports is None:
        from serial.tools import list_ports as lp
        comports = lp.comports()
    out = []
    for p in comports:
        out.append(Port(device=p.device, vid=getattr(p, "vid", None), pid=getattr(p, "pid", None),
                        serial=getattr(p, "serial_number", None), product=getattr(p, "product", None),
                        location=getattr(p, "location", None)))
    # ESP-looking ports first, then USB ports of unknown vendor, then the
    # motherboard's ttyS* — a USB id nobody recognises is still more likely to
    # be a node than a port with no USB behind it at all.
    out.sort(key=lambda p: (p.kind == "legacy", p.kind == "unknown", p.device))
    return out


def same_device(a: str, b: str) -> bool:
    """/dev/serial/by-id/... and /dev/ttyACM0 are one port; pyserial reports the
    latter and people rightly pass the former."""
    return a == b or os.path.realpath(a) == os.path.realpath(b)


def select_port(ports: list[Port], device: Optional[str] = None, serial: Optional[str] = None) -> Optional[Port]:
    """Pick one port by path or by USB serial number; None when ambiguous or absent."""
    if device:
        for p in ports:
            if same_device(p.device, device):
                return p
        return None
    if serial:
        hits = [p for p in ports if p.serial == serial]
        return hits[0] if len(hits) == 1 else None
    candidates = [p for p in ports if p.kind not in ("unknown", "legacy")]
    return candidates[0] if len(candidates) == 1 else None


# ---------------------------------------------------------------------------
# The maintenance console
# ---------------------------------------------------------------------------
class Console:
    """One request/reply exchange at a time on an open serial port. The
    transport is injectable: anything with read()/write()/in_waiting."""

    def __init__(self, ser, timeout: float = 2.0):
        self.ser = ser
        self.timeout = timeout

    @staticmethod
    def open(device: str, timeout: float = 2.0) -> "Console":
        import serial
        # The lines are left asserted, deliberately. The kernel raises DTR and
        # RTS together when the port opens, and pyserial then applies whatever
        # was asked for — DTR first, RTS second. Asking for both low therefore
        # passes through DTR low with RTS still high, which is precisely the
        # reset state esptool uses (RTS -> EN, DTR -> BOOT on a bridge, and the
        # USB-Serial/JTAG unit emulates the same). Both high is "running", and
        # on close the kernel drops both at once. So: touch neither.
        ser = serial.Serial()
        ser.port = device
        ser.baudrate = CONSOLE_BAUD
        ser.timeout = 0.2
        ser.dtr = True
        ser.rts = True
        ser.open()
        return Console(ser, timeout)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, deadline: float) -> Optional[str]:
        buf = bytearray()
        while time.monotonic() < deadline:
            b = self.ser.read(1)
            if not b:
                continue
            if b in (b"\n", b"\r"):
                if buf:
                    return buf.decode("utf-8", "replace")
                continue
            buf += b
            if len(buf) > 512:
                buf.clear()
        return None

    def command(self, line: str) -> tuple[str, dict, list[dict]]:
        """Send one command. Returns (status, kv, data_lines) where status is
        "OK", "ERR" or "TIMEOUT"; kv are the key=value pairs of the final line
        (for ERR: code and text); data_lines are the RM <CMD> lines before it."""
        cmd = line.split()[0].upper()
        self.ser.write((line.strip() + "\n").encode())
        self.ser.flush()
        deadline = time.monotonic() + self.timeout
        data = []
        while True:
            reply = self._readline(deadline)
            if reply is None:
                return "TIMEOUT", {}, data
            if not reply.startswith(REPLY_PREFIX):
                continue                       # a log line sharing the port
            words = reply[len(REPLY_PREFIX):]
            if words.startswith("OK " + cmd):
                return "OK", parse_kv(words[len("OK " + cmd):]), data
            if words.startswith("ERR "):
                m = re.match(r"ERR (\S+) (\d+) ?(.*)", words)
                if m and (m.group(1) == cmd or m.group(1) == "?"):
                    return "ERR", {"code": int(m.group(2)), "text": m.group(3)}, data
                continue
            if words.startswith(cmd + " "):
                data.append(parse_kv(words[len(cmd) + 1:]))


def parse_kv(text: str) -> dict:
    """key=value pairs; values may be double-quoted."""
    out = {}
    for m in re.finditer(r'(\w+)=("([^"]*)"|(\S+))', text):
        out[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(4)
    return out


@dataclass
class NodeInfo:
    firmware: str
    version: str
    board: str
    via: str                          # "console:/dev/ttyACM0" or "http://10.42.0.1"

    def __str__(self) -> str:
        return f"{self.firmware} {self.version} on {self.board} ({self.via})"


def probe_console(device: str, timeout: float = 2.0) -> Optional[NodeInfo]:
    """Ask VERSION on a port; None if nothing RetiMesh answers."""
    try:
        con = Console.open(device, timeout)
    except Exception:
        return None
    try:
        status, _, data = con.command("VERSION")
        if status != "OK" or not data:
            return None
        d = data[0]
        if d.get("firmware") != RETIMESH_PRODUCT:
            return None
        return NodeInfo(d["firmware"], d.get("version", "?"), d.get("board", "?"), f"console:{device}")
    finally:
        con.close()


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
def _http(url: str, body: Optional[dict] = None, auth: Optional[tuple[str, str]] = None,
          timeout: float = 3.0) -> tuple[int, dict]:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method="POST" if data else "GET",
                                 headers={"Content-Type": "application/json"} if data else {})
    if auth:
        import base64
        req.add_header("Authorization", "Basic " + base64.b64encode(f"{auth[0]}:{auth[1]}".encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except Exception:
            return e.code, {}
    except Exception:
        return 0, {}


def probe_http(base_url: str, timeout: float = 3.0, fetch=_http) -> Optional[NodeInfo]:
    code, doc = fetch(base_url.rstrip("/") + "/api/status", timeout=timeout)
    if code != 200 or doc.get("firmware") != RETIMESH_PRODUCT:
        return None
    return NodeInfo(doc["firmware"], doc.get("version", "?"), (doc.get("power") or {}).get("board", "?"), base_url)


def _int(v, default: int) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def request_bootloader_http(base_url: str, auth: tuple[str, str] = ("admin", "retimesh"),
                            fetch=_http) -> tuple[bool, str, int]:
    """(accepted, message, delay_ms the node said it would wait)."""
    code, doc = fetch(base_url.rstrip("/") + "/api/system/bootloader", {"confirm": "BOOTLOADER"}, auth)
    if code == 202:
        return True, f"accepted ({doc.get('method')}, {doc.get('delay_ms')} ms)", _int(doc.get("delay_ms"), 600)
    if code == 0:
        return False, "no HTTP answer", 0
    return False, f"HTTP {code}: {doc.get('error', 'refused')}", 0


# ---------------------------------------------------------------------------
# The hand-off
# ---------------------------------------------------------------------------
@dataclass
class HandOff:
    entered: bool
    method: str                       # "console", "http", "auto_reset_dtr_rts", "none"
    port: Optional[str]
    message: str
    tried: list[str] = field(default_factory=list)


def wait_for_port(predicate: Callable[[list[Port]], Optional[Port]], timeout: float,
                  ports_fn=list_ports, sleep=time.sleep, clock=time.monotonic) -> Optional[Port]:
    deadline = clock() + timeout
    while True:
        hit = predicate(ports_fn())
        if hit or clock() >= deadline:
            return hit
        sleep(0.25)


def hand_off_to_bootloader(port: Optional[str] = None, node_url: Optional[str] = None,
                           auth: tuple[str, str] = ("admin", "retimesh"), log: Log = _quiet,
                           ports_fn=list_ports, probe=probe_console,
                           request_http=request_bootloader_http, sleep=time.sleep,
                           clock=time.monotonic, reappear_timeout: float = 8.0) -> HandOff:
    """Console first, HTTP second, esptool's own reset last.

    Returns where esptool should point and whether the downloader is known to
    be up. `entered` is False when the board is left to esptool's DTR/RTS
    reset, which is the normal outcome on a bridge or USB-Serial/JTAG port and
    not a failure."""
    tried: list[str] = []
    ports = ports_fn()
    chosen = select_port(ports, device=port) if port else select_port(ports)
    if port and chosen is None:
        return HandOff(False, "none", port, f"{port} is not present; nothing to hand off", tried)
    if chosen is None:
        eligible = [p for p in ports if p.kind not in ("unknown", "legacy")]
        if len(eligible) > 1:
            return HandOff(False, "none", None,
                           "several ESP32 ports: " + "; ".join(p.label() for p in eligible) +
                           " — choose one with --upload-port", tried)
        if not eligible and not node_url:
            return HandOff(False, "none", None, "no ESP32 serial port found and no RETIMESH_NODE_URL set", tried)

    # 1. Console. Fast, credential-free, and it names the board.
    if chosen:
        tried.append(f"console on {chosen.device}")
        info = probe(chosen.device)
        if info:
            log(f"found {info}")
            try:
                con = Console.open(chosen.device)
                status, kv, _ = con.command("BOOTLOADER CONFIRM")
                con.close()
            except Exception as exc:
                status, kv = "ERR", {"code": 0, "text": str(exc)}
            if status == "OK":
                log(f"node accepted BOOTLOADER ({kv.get('method')}, {kv.get('delay_ms')} ms)")
                return _await_downloader(chosen, "console", log, ports_fn, sleep, clock, reappear_timeout, tried,
                                         delay_ms=_int(kv.get("delay_ms"), 600))
            if status == "ERR" and kv.get("code") == 501:
                log(f"this board cannot enter its downloader from software: {kv.get('text')}")
                return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                               "leaving the reset to esptool (bridge DTR/RTS)", tried)
            log(f"console refused ({status} {kv}); trying HTTP")
        else:
            log(f"no RetiMesh console on {chosen.device}")

    # 2. HTTP, when a URL is known.
    if node_url:
        tried.append(f"http {node_url}")
        ok, msg, delay_ms = request_http(node_url, auth)
        log(f"HTTP bootloader request: {msg}")
        if ok:
            target = chosen or wait_for_port(
                lambda ps: select_port(ps), reappear_timeout, ports_fn, sleep, clock)
            if target:
                return _await_downloader(target, "http", log, ports_fn, sleep, clock, reappear_timeout, tried,
                                         delay_ms=delay_ms)
            return HandOff(True, "http", None, "downloader requested but no serial port appeared", tried)

    # 3. Nothing worked, or nothing needed to: esptool resets what it can.
    if chosen and chosen.auto_reset:
        return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                       f"leaving the reset to esptool on {chosen.device} ({chosen.kind})", tried)
    return HandOff(False, "none", chosen.device if chosen else None,
                   "could not reach a RetiMesh node; if the upload fails, hold BOOT and press RST, then retry",
                   tried)


def _await_downloader(port: Port, method: str, log: Log, ports_fn, sleep, clock, timeout: float,
                      tried: list[str], delay_ms: int = 600) -> HandOff:
    """After a software request the S3's USB-Serial/JTAG unit disappears for a
    moment and comes back as the same VID:PID; a bridge port never goes away.
    Either way the port has to be present and quiet, and there is no
    device-side signal beyond that — esptool's sync is the real test."""
    sleep(delay_ms / 1000.0 + 0.3)                          # the node's own ack delay, then the reset
    back = wait_for_port(lambda ps: select_port(ps, device=port.device), timeout, ports_fn, sleep, clock)
    if back is None:
        # It may have come back under a new name (ttyACM0 -> ttyACM1 on a
        # busy host); accept a single port of the same kind.
        back = wait_for_port(lambda ps: _same_kind(ps, port), 2.0, ports_fn, sleep, clock)
    if back is None:
        return HandOff(False, method, port.device,
                       f"{port.device} did not reappear within {timeout:.0f} s after the bootloader request; "
                       "hold BOOT, press RST, then retry", tried)
    return HandOff(True, method, back.device, "downloader port present", tried)


def _same_kind(ports: list[Port], like: Port) -> Optional[Port]:
    hits = [p for p in ports if p.kind == like.kind]
    return hits[0] if len(hits) == 1 else None


def listen_for_hello(device: str, timeout: float) -> bool:
    """Hold the port open and wait for the firmware's boot banner ("RM HELLO",
    or any reply line). One open, one wait: re-opening every second would
    re-raise the modem lines on a node that is still booting."""
    try:
        con = Console.open(device, timeout)
    except Exception:
        return False
    try:
        deadline = time.monotonic() + timeout
        while True:
            line = con._readline(deadline)
            if line is None:
                return False
            if line.startswith(REPLY_PREFIX):
                return True
    finally:
        con.close()


def wait_for_application(port: Optional[str], timeout: float, log: Log = _quiet,
                         probe=probe_console, ports_fn=list_ports, sleep=time.sleep,
                         clock=time.monotonic, hello=listen_for_hello) -> Optional[NodeInfo]:
    """After a flash: the port comes back, the banner appears, VERSION answers."""
    deadline = clock() + timeout
    while clock() < deadline:
        ports = ports_fn()
        target = select_port(ports, device=port) if port else select_port(ports)
        if target:
            remaining = max(1.0, deadline - clock())
            if hello(target.device, remaining):
                info = probe(target.device)
                if info:
                    return info
        sleep(1.0)
    return None
