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
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

# USB identities. The S3's fixed USB-Serial/JTAG unit is the same device in
# the ROM downloader and in an application that leaves it in charge. A node
# whose own USB is driven by the OTG stack presents the composite device
# instead — CDC-ACM console and CDC-NCM network, docs/local-link.md — under
# the pair boards.json's _usb_identity names; it is copied here because this
# package installs on its own, and tests/test_device.py holds the two to each
# other. The composite device's downloader is the serial-JTAG pair again: the
# firmware hands the peripheral back to that unit before it restarts.
ESP_USB_SERIAL_JTAG = (0x303A, 0x1001)
RETIMESH_COMPOSITE = (0x1209, 0x0001)
BRIDGES = {(0x10C4, 0xEA60): "CP2102", (0x1A86, 0x55D4): "CH9102", (0x1A86, 0x7523): "CH340",
           (0x0403, 0x6001): "FT232", (0x0403, 0x6015): "FT231X"}
# Vendors whose unlisted products are still, almost always, an ESP32 or the
# bridge in front of one: Espressif, Silicon Labs, WCH, FTDI. A CH343 or an
# FT2232 is not in the table above and should not be treated as a stranger.
ESP_VENDORS = {0x303A, 0x10C4, 0x1A86, 0x0403}
RETIMESH_PRODUCT = "RetiMesh Node"

CONSOLE_BAUD = 115200
REPLY_PREFIX = "RM "
# The admin credentials the firmware ships with (ADMIN_USER / ADMIN_PASSWORD_DEFAULT
# in the firmware). Written once here; the CLI's --password default and both
# HTTP callers read it.
DEFAULT_ADMIN = ("admin", "retimesh")

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
        if (self.vid, self.pid) == RETIMESH_COMPOSITE:
            return "retimesh_composite"
        return BRIDGES.get((self.vid, self.pid), "unknown" if self.vid is not None else "legacy")

    @property
    def likely_esp(self) -> bool:
        """Whether this port is worth asking. One predicate, because five call
        sites used to each spell out which kinds to skip."""
        return self.kind not in ("unknown", "legacy") or self.vid in ESP_VENDORS

    @property
    def auto_reset(self) -> bool:
        """Whether esptool's DTR/RTS reset is expected to work on this port. It
        is the norm on every bridge these vendors make and on the S3's own
        unit, so an unlisted product from one of them gets the benefit. The
        composite device is the exception: its ACM port honours the same
        DTR/RTS pattern by restarting into the ROM — which then enumerates as
        the serial-JTAG unit, another port — so esptool would be left
        resetting a port that has gone. The hand-off does that transition
        itself and points esptool at the port that appears."""
        return self.likely_esp and self.kind != "retimesh_composite"

    @property
    def node_id(self) -> Optional[str]:
        """The chip's MAC as this port reports it, colons dropped and upper-cased,
        so the composite device (1CDBD4821454) and the serial-JTAG unit
        (1C:DB:D4:82:14:54) of one chip compare equal."""
        return self.serial.replace(":", "").upper() if self.serial else None

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
        # pyserial's `product` is frequently None on Windows; `description`
        # is what it shows in its own listing and is always there.
        product = getattr(p, "product", None) or getattr(p, "description", None)
        out.append(Port(device=p.device, vid=getattr(p, "vid", None), pid=getattr(p, "pid", None),
                        serial=getattr(p, "serial_number", None), product=product,
                        location=getattr(p, "location", None)))
    # ESP-looking ports first, then USB ports of unknown vendor, then the
    # motherboard's ttyS* — a USB id nobody recognises is still more likely to
    # be a node than a port with no USB behind it at all.
    out.sort(key=lambda p: (p.kind == "legacy", not p.likely_esp, p.device))
    return out


def esp_candidates(ports: list[Port]) -> list[Port]:
    return [p for p in ports if p.likely_esp]


def ambiguous_ports_message(ports: list[Port], hint: str) -> str:
    """One wording for "which of these?", with the caller's own flag named."""
    return ("several ESP32 ports: " + "; ".join(p.label() for p in esp_candidates(ports))
            + f" — choose one with {hint}")


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
    candidates = esp_candidates(ports)
    return candidates[0] if len(candidates) == 1 else None


def node_id_from_path(device: str) -> Optional[str]:
    """The MAC a /dev/serial/by-id name carries, colons dropped and upper-cased,
    for either face of the chip: usb-RetiMesh_RetiMesh_Node_1CDBD4821454-if00
    or usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:82:14:54-if00."""
    m = re.search(r"(?:RetiMesh_Node_|serial_debug_unit_)([0-9A-Fa-f:]{12,17})-if", device)
    return m.group(1).replace(":", "").upper() if m else None


def usb_node_url(node_id: str) -> str:
    """Where a node answers over its USB link, from its MAC: 10.64.<n>.1 with
    <n> the last octet — the firmware's usbNodeAddress() (LocalLinkState.h),
    written here a second time because this package cannot include that
    header; the tests hold both to the same vector."""
    return f"http://10.64.{int(node_id[-2:], 16)}.1"


def touch_1200(device: str, sleep=time.sleep) -> None:
    """The 1200-baud touch: opening the composite device's ACM port at 1200
    baud with DTR and RTS low makes the core restart into the ROM downloader
    (its USBCDC honours the Arduino convention). The port vanishes under the
    open, which pyserial reports as an error that here means success."""
    import serial
    ser = serial.Serial()
    ser.port = device
    ser.baudrate = 1200
    ser.dtr = False
    ser.rts = False
    try:
        ser.open()
        sleep(0.2)
        ser.close()
    except Exception:
        pass


def node_url(text: str) -> str:
    """An address as typed -> a base URL: scheme added, trailing slash dropped.
    One place, because the CLI, the hook and the HTTP helpers each did half of
    this and RETIMESH_NODE_URL=10.42.0.1 reached the request unnormalised."""
    text = text.strip()
    if not re.match(r"^https?://", text):
        text = "http://" + text
    return text.rstrip("/")


# ---------------------------------------------------------------------------
# The maintenance console
# ---------------------------------------------------------------------------
class Console:
    """One request/reply exchange at a time on an open serial port. The
    transport is injectable: anything with read()/write()/in_waiting. So is
    the clock, so a test can run a timeout without patching the stdlib for
    every other thread in the process."""

    def __init__(self, ser, timeout: float = 2.0, device: Optional[str] = None, clock=time.monotonic):
        self.ser = ser
        self.timeout = timeout
        self.device = device or getattr(ser, "port", None) or "?"
        self.clock = clock
        self._pending = bytearray()

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
        # Nothing is done here about the S3's habit of losing the last USB
        # packet it held when the port was opened: the node begins every
        # reply on a fresh line, so a log line that lost its newline cannot
        # swallow the first reply line, and every OK line says how many data
        # lines came before it, so command() sees a short reply and asks
        # again. An earlier version warmed the session with a HELP nobody
        # needed, which cost every probe of a silent port a timeout and still
        # could not tell a whole reply from a short one.
        return Console(ser, timeout, device)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, deadline: float) -> Optional[str]:
        # Whole chunks, not one byte per read: a HELP reply is several hundred
        # bytes, and one syscall per byte with a 200 ms port timeout also let
        # every idle line overshoot the deadline by that much. The remainder
        # after a newline is kept for the next call.
        while True:
            nl = -1
            for i, b in enumerate(self._pending):
                if b in (0x0A, 0x0D):
                    nl = i
                    break
            if nl >= 0:
                line = bytes(self._pending[:nl]); del self._pending[:nl + 1]
                if line:
                    return line.decode("utf-8", "replace")
                continue
            if len(self._pending) > 512:
                self._pending.clear()
            if self.clock() >= deadline:
                return None
            waiting = getattr(self.ser, "in_waiting", 0) or 0
            chunk = self.ser.read(max(1, min(waiting, 512)))
            if chunk:
                self._pending += chunk

    def command(self, line: str) -> tuple[str, dict, list[dict]]:
        """Send one command. Returns (status, kv, data_lines) where status is
        "OK", "ERR", "SHORT" or "TIMEOUT"; kv are the key=value pairs of the
        final line (for ERR: code and text); data_lines are the RM <CMD> lines
        before it.

        The node's OK line says how many data lines it sent (`lines=`, taken
        out of kv here). A reply with fewer is asked for again, once — a line
        can still be lost to the port — and a reply short on the second try
        is SHORT, its data handed back for what it is worth rather than passed
        off as complete. A node that does not count (protocol 1) is taken at
        its word."""
        for attempt in (1, 2):
            status, kv, data = self._exchange(line)
            if status == "ERR" and kv.get("cmd") == "?" and attempt == 1:
                # The node refused a line it could not even name: ours, glued
                # onto bytes a port prober left in its buffer. The node has
                # discarded them with that reply; once more and it is clean.
                continue
            if status != "OK":
                return status, kv, data
            expected = kv.pop("lines", None)
            if expected is None or not expected.isdigit() or int(expected) == len(data):
                return status, kv, data
        return "SHORT", kv, data

    def _exchange(self, line: str) -> tuple[str, dict, list[dict]]:
        cmd = line.split()[0].upper()
        self.ser.write((line.strip() + "\n").encode())
        self.ser.flush()
        deadline = self.clock() + self.timeout
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
                    return "ERR", {"cmd": m.group(1), "code": int(m.group(2)), "text": m.group(3)}, data
                continue
            if words.startswith(cmd + " "):
                data.append(parse_kv(words[len(cmd) + 1:]))

    def version(self) -> Optional["NodeInfo"]:
        """VERSION on this session; None unless a RetiMesh node answers."""
        status, _, data = self.command("VERSION")
        if status != "OK" or not data:
            return None
        d = data[0]
        if d.get("firmware") != RETIMESH_PRODUCT:
            return None
        return NodeInfo(d["firmware"], d.get("version", "?"), d.get("board", "?"), f"console:{self.device}")


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


def probe_console(device: str, timeout: float = 2.0, console: Optional[Console] = None) -> Optional[NodeInfo]:
    """Ask VERSION on a port; None if nothing RetiMesh answers. Pass an open
    `console` to ask on a session the caller already holds, rather than
    opening the port a second time — which re-raises the modem lines the
    open() above goes to some length to leave alone. Nothing here raises: a
    port that vanishes mid-read is "no answer", not a traceback."""
    own = console is None
    try:
        con = console or Console.open(device, timeout)
    except Exception:
        return None
    try:
        return con.version()
    except Exception:
        return None
    finally:
        if own:
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


def _doc(value) -> dict:
    """Whatever the wire returned as an object, or nothing. A captive page or
    another device answering the path with a list, a string or null is a
    'not a node', not a traceback."""
    return value if isinstance(value, dict) else {}


def probe_http(base_url: str, timeout: float = 3.0, fetch=_http) -> Optional[NodeInfo]:
    base_url = node_url(base_url)
    code, doc = fetch(base_url + "/api/status", timeout=timeout)
    doc = _doc(doc)
    if code != 200 or doc.get("firmware") != RETIMESH_PRODUCT:
        return None
    return NodeInfo(doc["firmware"], doc.get("version", "?"), (doc.get("power") or {}).get("board", "?"), base_url)


def _int(v, default: int) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def request_bootloader_http(base_url: str, auth: tuple[str, str] = DEFAULT_ADMIN,
                            fetch=_http) -> tuple[bool, str, int]:
    """(accepted, message, delay_ms the node said it would wait)."""
    code, doc = fetch(node_url(base_url) + "/api/system/bootloader", {"confirm": "BOOTLOADER"}, auth)
    doc = _doc(doc)
    if code == 202:
        return True, f"accepted ({doc.get('method')}, {doc.get('delay_ms')} ms)", _int(doc.get("delay_ms"), 600)
    if code == 0:
        return False, "no HTTP answer", 0
    return False, f"HTTP {code}: {doc.get('error', 'refused')}", 0


# ---------------------------------------------------------------------------
# esptool
# ---------------------------------------------------------------------------
_ESPTOOL_MAJOR: Optional[int] = None


def esptool_major() -> int:
    """The installed esptool's major version, for the option spelling below;
    4 when it cannot be imported, which is the spelling PlatformIO's bundled
    copy uses. Looked up once: opt() runs for every argument."""
    global _ESPTOOL_MAJOR
    if _ESPTOOL_MAJOR is None:
        try:
            import esptool
            _ESPTOOL_MAJOR = int(esptool.__version__.split(".")[0])
        except Exception:
            _ESPTOOL_MAJOR = 4
    return _ESPTOOL_MAJOR


def opt(name: str) -> str:
    """esptool >= 5 spells options and commands with dashes; 4.x only
    accepts underscores. Emit whichever the installed version wants."""
    return name.replace("_", "-") if esptool_major() >= 5 else name


def esptool_args(chip: Optional[str], port: str, before: str, after: str, *cmd, baud: Optional[int] = None) -> list[str]:
    """The argument list esptool gets, built once. The CLI, the HIL runner and
    the check below all invoke esptool; a second builder beside this one is
    how the spelling rule for esptool 4 and 5 came to be applied by hand in
    two places."""
    args = ["--chip", chip] if chip else []
    args += ["--port", port]
    if baud:
        args += ["--baud", str(baud)]
    args += ["--before", opt(before), "--after", opt(after), opt(cmd[0]), *cmd[1:]]
    return args


# How esptool is invoked. The CLI runs in a venv that has esptool as a
# package; the PlatformIO hook runs in PlatformIO's own interpreter, which
# does not, and hands in the path of the bundled esptool.py instead.
DEFAULT_ESPTOOL_CMD = [sys.executable, "-m", "esptool"]


def downloader_present(device: str, timeout: float = 15.0, esptool_cmd: Optional[list[str]] = None) -> bool:
    """Whether a ROM downloader answers on the port, asked the one way that is
    definitive: esptool's own sync, with no reset before or after. A silent
    console cannot tell a downloader from a dead node, and the difference
    decides what esptool must be told — a DTR/RTS reset performed on a chip
    already in its downloader re-enumerates the port under esptool's own
    open, which is how a node left in the downloader by one failed flash
    failed the next one too."""
    import subprocess
    try:
        rc = subprocess.run([*(esptool_cmd or DEFAULT_ESPTOOL_CMD),
                             *esptool_args(None, device, "no_reset", "no_reset", "chip_id")],
                            capture_output=True, text=True, timeout=timeout)
    except Exception:
        return False
    return rc.returncode == 0


# ---------------------------------------------------------------------------
# The hand-off
# ---------------------------------------------------------------------------
@dataclass
class HandOff:
    entered: bool                     # the ROM downloader is known to be up on `port`
    method: str                       # "console", "touch", "http", "downloader", "auto_reset_dtr_rts", "none"
    port: Optional[str]
    message: str
    reset_capable: bool = True        # esptool's DTR/RTS reaches EN/BOOT (or the serial-JTAG unit) on `port`

    @property
    def esptool_before(self) -> str:
        """What to tell esptool. Decided here once: the CLI, the PlatformIO hook
        and the HIL script each used to derive it from the fields above.

        esptool's own reset at connect is skipped only where the port cannot
        drive one and the downloader is already there. Where it can, it is
        run even on a downloader that is already up: on the S3's serial-JTAG
        unit a ROM entered from software stays in the downloader through
        esptool's closing hard reset unless esptool's connect-time sequence
        ran first — measured, and the bench spent an afternoon on it."""
        return "no_reset" if self.entered and not self.reset_capable else "default_reset"


def wait_for_port(predicate: Callable[[list[Port]], Optional[Port]], timeout: float,
                  ports_fn=list_ports, sleep=time.sleep, clock=time.monotonic,
                  interval: float = 0.25) -> Optional[Port]:
    deadline = clock() + timeout
    while True:
        hit = predicate(ports_fn())
        if hit or clock() >= deadline:
            return hit
        sleep(interval)


def hand_off_to_bootloader(port: Optional[str] = None, node_url_text: Optional[str] = None,
                           auth: tuple[str, str] = DEFAULT_ADMIN, log: Log = _quiet,
                           ports_fn=list_ports, probe=probe_console, open_console=Console.open,
                           request_http=request_bootloader_http, probe_rom=None,
                           esptool_cmd: Optional[list[str]] = None,
                           sleep=time.sleep, clock=time.monotonic, reappear_timeout: float = 8.0,
                           port_hint: str = "--port", touch=touch_1200) -> HandOff:
    """Console first, HTTP second, esptool's own reset last.

    Returns where esptool should point and whether the downloader is known to
    be up. `entered` is False when the board is left to esptool's DTR/RTS
    reset, which is the normal outcome on a bridge or USB-Serial/JTAG port and
    not a failure."""
    if probe_rom is None:
        probe_rom = lambda dev: downloader_present(dev, esptool_cmd=esptool_cmd)  # noqa: E731
    ports = ports_fn()
    chosen = select_port(ports, device=port)
    if port and chosen is None:
        # The named port is not there. If it was the composite device and the
        # chip is already in its ROM — an earlier attempt got it there and
        # then lost track — its serial-JTAG downloader is on the bus under
        # the same MAC, which the by-id name carries.
        node_id = node_id_from_path(port)
        rom = _port_of(ports, "usb_serial_jtag", node_id) if node_id else None
        if rom and probe_rom(rom.device):
            log(f"{port} is gone, but the chip's serial-JTAG downloader answers on {rom.device}")
            return HandOff(True, "downloader", rom.device, "the downloader is already on the port",
                           reset_capable=rom.auto_reset)
        # Otherwise: mid-re-enumeration, or a by-id link that is momentarily
        # gone. That is no reason to skip the HTTP path, which is precisely
        # the one that does not need the port; an earlier version gave up
        # here with RETIMESH_NODE_URL unread.
        if not node_url_text:
            return HandOff(False, "none", port, f"{port} is not present; nothing to hand off")
        log(f"{port} is not present; trying HTTP")
    elif chosen is None:
        eligible = esp_candidates(ports)
        if len(eligible) > 1:
            return HandOff(False, "none", None, ambiguous_ports_message(ports, port_hint))
        if not eligible and not node_url_text:
            return HandOff(False, "none", None, "no ESP32 serial port found and no RETIMESH_NODE_URL set")

    # 1. Console. Fast, credential-free, and it names the board. One session
    #    for the question and the request.
    if chosen:
        con = None
        try:
            con = open_console(chosen.device)
        except Exception as exc:
            log(f"could not open {chosen.device}: {exc}")
        info = probe(chosen.device, console=con) if con else None
        if info:
            log(f"found {info}")
            try:
                status, kv, _ = con.command("BOOTLOADER CONFIRM")
            except Exception as exc:
                status, kv = "ERR", {"code": 0, "text": str(exc)}
            con.close()
            if status == "OK":
                log(f"node accepted BOOTLOADER ({kv.get('method')}, {kv.get('delay_ms')} ms)")
                return _await_downloader(chosen, "console", log, ports_fn, probe, probe_rom, sleep, clock,
                                         reappear_timeout, delay_ms=_int(kv.get("delay_ms"), 600))
            if status == "ERR" and kv.get("code") == 501:
                if chosen.kind == "retimesh_composite":
                    # A firmware that presents the composite device but has
                    # not learnt to enter from its console; the core's own
                    # touch does it regardless.
                    log("the console does not offer software entry; using the 1200-baud touch")
                    touch(chosen.device)
                    return _await_downloader(chosen, "touch", log, ports_fn, probe, probe_rom, sleep, clock,
                                             reappear_timeout)
                log(f"this board cannot enter its downloader from software: {kv.get('text')}")
                return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                               "leaving the reset to esptool (bridge DTR/RTS)")
            log(f"console refused ({status} {kv}); trying HTTP")
        else:
            if con:
                con.close()
            log(f"no RetiMesh console on {chosen.device}")
            if chosen.kind == "retimesh_composite":
                # The console may be switched off; the touch needs no console.
                log("using the 1200-baud touch")
                touch(chosen.device)
                return _await_downloader(chosen, "touch", log, ports_fn, probe, probe_rom, sleep, clock,
                                         reappear_timeout)
            # Silent is what a ROM downloader sounds like too — a node left
            # there by an earlier attempt, or by somebody's BOOT button.
            if probe_rom(chosen.device):
                log("a ROM downloader is already answering on the port")
                return HandOff(True, "downloader", chosen.device, "a ROM downloader is already on the port",
                               reset_capable=chosen.auto_reset)

    # 2. HTTP, when a URL is known — and over the USB link it always is.
    if not node_url_text and chosen and chosen.kind == "retimesh_composite" and chosen.node_id:
        node_url_text = usb_node_url(chosen.node_id)
    if node_url_text:
        ok, msg, delay_ms = request_http(node_url_text, auth)
        log(f"HTTP bootloader request: {msg}")
        if ok:
            target = chosen or wait_for_port(lambda ps: select_port(ps), reappear_timeout, ports_fn, sleep, clock)
            if target:
                return _await_downloader(target, "http", log, ports_fn, probe, probe_rom, sleep, clock,
                                         reappear_timeout, delay_ms=delay_ms)
            # Requested, but nothing to point esptool at: not "entered". The
            # field promises a port the downloader is known to be up on, and
            # the hook turns it into no_reset for whatever port PlatformIO
            # finds next — a promise nobody checked.
            return HandOff(False, "http", None, "downloader requested but no serial port appeared; "
                           "hold BOOT, press RST, then retry")

    # 3. Nothing worked, or nothing needed to: esptool resets what it can.
    if chosen and chosen.auto_reset:
        return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                       f"leaving the reset to esptool on {chosen.device} ({chosen.kind})")
    return HandOff(False, "none", chosen.device if chosen else None,
                   "could not reach a RetiMesh node; if the upload fails, hold BOOT and press RST, then retry")


# How long after its acknowledgement the node may take to actually reset:
# the ack delay it named, then up to two passes of its 200 ms loop before the
# restart step runs, then the shutdown handlers. Generous, because the cost of
# waiting a little longer is a little waiting, and the cost of giving up early
# is telling esptool to reset a chip that is already in its downloader.
_RESTART_SLACK_S = 1.5

# How long a composite device's downloader may take to show up. The chip is
# in its ROM within two seconds of the request; whether the host learns of
# it is another matter. A root port notices the change at once. Some hubs
# do not report a full-speed device's departure until a transfer to it
# fails, which can be minutes later — one bench hub took anything from
# three seconds to two and a half minutes, whatever the device did on the
# wire — and the serial-JTAG unit is only enumerated after that. Waiting
# is the whole cure: the ROM sits there for as long as it takes.
_COMPOSITE_DOWNLOADER_S = 180.0


def _await_downloader(port: Port, method: str, log: Log, ports_fn, probe, probe_rom, sleep, clock,
                      timeout: float, delay_ms: int = 600) -> HandOff:
    """Confirm the node actually went down and came back as the downloader.

    On the S3's USB-Serial/JTAG unit a port that drops and re-enumerates
    is proof of a reset, and it is watched for from the moment of the
    acknowledgement. But the unit survives the *software* reset the firmware
    performs — only a reset through EN re-enumerates it — so on the bench the
    port simply never goes away, and an earlier version, which took "still
    present" to mean "never reset", handed esptool a DTR/RTS reset to perform
    on a chip already sitting in its downloader. The re-enumeration that
    caused landed in the middle of esptool opening the port.

    So presence proves nothing, and the console is asked: a running
    application answers VERSION, a downloader does not. Silence is not the
    last word either — a dead node is silent too — so the final test in every
    path is esptool's own sync with no reset, which only a downloader passes.
    On a bridge, whose port belongs to the bridge and never moves, those two
    questions are the whole test."""
    window = delay_ms / 1000.0 + _RESTART_SLACK_S
    if port.kind == "retimesh_composite":
        # The composite device goes away with the restart and the ROM comes
        # up on the chip's serial-JTAG unit: another port, known to be the
        # same chip by the MAC both report.
        log(f"watching for {port.device} to go and the serial-JTAG downloader to come")
        gone = wait_for_port(lambda ps: None if _port_of(ps, "retimesh_composite", port.node_id) else port,
                             window, ports_fn, sleep, clock, interval=0.1)
        if gone is None and probe(port.device, timeout=1.0):
            return HandOff(False, "none", port.device,
                           "the console still answers after the request, so the node did not reset; "
                           "hold BOOT, press RST, then retry")
        if gone is None:
            log("the node has stopped answering; its port will drop when the host notices — "
                "at once on a root port, up to a minute behind some hubs")
        started = clock()
        back = None
        while back is None and clock() - started < _COMPOSITE_DOWNLOADER_S:
            back = wait_for_port(lambda ps: _port_of(ps, "usb_serial_jtag", port.node_id), 15.0, ports_fn, sleep, clock)
            if back is None:
                log(f"still waiting for the serial-JTAG downloader ({clock() - started:.0f} s)")
        if back is None:
            return HandOff(False, method, None,
                           f"the serial-JTAG downloader did not appear within {_COMPOSITE_DOWNLOADER_S:.0f} s after the request; "
                           "hold BOOT, press RST, then retry")
        log(f"the downloader is on {back.device}")
        return _confirm_downloader(back.device, method, log, probe_rom, reset_capable=back.auto_reset)
    gone = None
    if port.kind == "usb_serial_jtag":
        log(f"watching {port.device} for the reset")
        gone = wait_for_port(lambda ps: None if select_port(ps, device=port.device) else port,
                             window, ports_fn, sleep, clock, interval=0.1)
    else:
        sleep(window)
    if gone is None:
        # No drop seen — normal on a bridge, and on the S3 the usual case
        # after a software reset. The console decides: a running application
        # answers, a downloader does not.
        log(f"checking that the console on {port.device} has gone quiet")
        if probe(port.device, timeout=1.0):
            return HandOff(False, "auto_reset_dtr_rts", port.device,
                           "the console still answers after the request, so the node did not reset; "
                           "leaving the reset to esptool")
        log("the console is silent; asking esptool")
        return _confirm_downloader(port.device, method, log, probe_rom, reset_capable=port.auto_reset)
    log(f"{port.device} went away; waiting for it to come back")
    back = wait_for_port(lambda ps: select_port(ps, device=port.device), timeout, ports_fn, sleep, clock)
    if back is None:
        # It may have come back under a new name (ttyACM0 -> ttyACM1 on a
        # busy host); accept a single port of the same kind.
        back = wait_for_port(lambda ps: _same_kind(ps, port), 2.0, ports_fn, sleep, clock)
    if back is None:
        return HandOff(False, method, port.device,
                       f"{port.device} did not reappear within {timeout:.0f} s after the bootloader request; "
                       "hold BOOT, press RST, then retry")
    return _confirm_downloader(back.device, method, log, probe_rom, reset_capable=back.auto_reset)


def _confirm_downloader(device: str, method: str, log: Log, probe_rom, reset_capable: bool = True) -> HandOff:
    if probe_rom(device):
        return HandOff(True, method, device, "downloader confirmed by esptool", reset_capable=reset_capable)
    log(f"no downloader answers on {device}; leaving the reset to esptool")
    return HandOff(False, "auto_reset_dtr_rts", device,
                   "the port is back but no ROM downloader answers on it; leaving the reset to esptool")


def _port_of(ports: list[Port], kind: str, node_id: Optional[str]) -> Optional[Port]:
    """The one port of this kind belonging to this chip — or, when the chip's
    MAC is unknown, the one port of this kind at all."""
    hits = [p for p in ports if p.kind == kind and (node_id is None or p.node_id == node_id)]
    return hits[0] if len(hits) == 1 else None


def _same_kind(ports: list[Port], like: Port) -> Optional[Port]:
    hits = [p for p in ports if p.kind == like.kind]
    return hits[0] if len(hits) == 1 else None


def wait_for_application(port: Optional[str], timeout: float, log: Log = _quiet,
                         probe=probe_console, ports_fn=list_ports, sleep=time.sleep,
                         clock=time.monotonic) -> Optional[NodeInfo]:
    """After a flash: ask VERSION until it answers or time runs out.

    Asked, not listened for. The firmware prints its HELLO banner exactly
    once, early in boot, and on a native-USB board while the host has nothing
    reading the port that line is dropped by the CDC driver — so a host that
    opens the port a moment late would wait the whole timeout for a banner
    that had already gone by, as an earlier version did. VERSION answers
    whenever the application is up, whoever missed what.

    `port` is a hint. The application may come back under a different name
    from the downloader (ttyACM1 -> ttyACM0 once the busy host lets go), and
    with no hint at all there may be several ESP-looking ports; every
    candidate is asked, since VERSION is what tells a node from the rest."""
    deadline = clock() + timeout
    announced = set()
    # The chip that was flashed is known by the MAC in the port's name, on a
    # native-USB board; when its port comes back under another face (the
    # composite device after the serial-JTAG downloader), only that chip is
    # asked. Without it, a bench with several nodes answered with whichever
    # was first, and once reported a soak node as the application coming back.
    node_id = node_id_from_path(port) if port else None
    while clock() < deadline:
        ports = ports_fn()
        named = select_port(ports, device=port) if port else None
        same = [p for p in ports if node_id and p.node_id == node_id]
        targets = [named] if named else (same or (esp_candidates(ports) if not node_id else []))
        for target in targets:
            if target.device not in announced:
                log(f"asking VERSION on {target.device}")
                announced.add(target.device)
            info = probe(target.device, timeout=1.5)
            if info:
                return info
        sleep(1.0)
    return None
