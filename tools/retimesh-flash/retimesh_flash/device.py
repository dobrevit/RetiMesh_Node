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
# the application and in the ROM downloader. A RetiMesh composite device would
# be a second pair, listed when it ships.
ESP_USB_SERIAL_JTAG = (0x303A, 0x1001)
BRIDGES = {(0x10C4, 0xEA60): "CP2102", (0x1A86, 0x55D4): "CH9102", (0x1A86, 0x7523): "CH340",
           (0x0403, 0x6001): "FT232", (0x0403, 0x6015): "FT231X"}
# Vendors whose unlisted products are still, almost always, an ESP32 or the
# bridge in front of one: Espressif, Silicon Labs, WCH, FTDI. A CH343 or an
# FT2232 is not in the table above and should not be treated as a stranger.
ESP_VENDORS = {0x303A, 0x10C4, 0x1A86, 0x0403}
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
    def likely_esp(self) -> bool:
        """Whether this port is worth asking. One predicate, because five call
        sites used to each spell out which kinds to skip."""
        return self.kind not in ("unknown", "legacy") or self.vid in ESP_VENDORS

    @property
    def auto_reset(self) -> bool:
        """Whether esptool's DTR/RTS reset is expected to work on this port. It
        is the norm on every bridge these vendors make and on the S3's own
        unit, so an unlisted product from one of them gets the benefit."""
        return self.likely_esp

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
        # A moment after opening before anything is sent. The S3's USB CDC
        # does not count the host as connected until shortly after the port
        # opens, and a reply written in that window is dropped — measured on
        # the bench as the first line of a STATUS answer going missing when
        # the command followed the open immediately, and never once a tenth
        # of a second had passed. A quarter of a second is margin, not cost.
        time.sleep(0.25)
        ser.reset_input_buffer()
        return Console(ser, timeout, device)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, deadline: float) -> Optional[str]:
        buf = bytearray()
        while self.clock() < deadline:
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
                    return "ERR", {"code": int(m.group(2)), "text": m.group(3)}, data
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


def probe_http(base_url: str, timeout: float = 3.0, fetch=_http) -> Optional[NodeInfo]:
    base_url = node_url(base_url)
    code, doc = fetch(base_url + "/api/status", timeout=timeout)
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
    code, doc = fetch(node_url(base_url) + "/api/system/bootloader", {"confirm": "BOOTLOADER"}, auth)
    if code == 202:
        return True, f"accepted ({doc.get('method')}, {doc.get('delay_ms')} ms)", _int(doc.get("delay_ms"), 600)
    if code == 0:
        return False, "no HTTP answer", 0
    return False, f"HTTP {code}: {doc.get('error', 'refused')}", 0


# ---------------------------------------------------------------------------
# esptool
# ---------------------------------------------------------------------------
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
    method: str                       # "console", "http", "downloader", "auto_reset_dtr_rts", "none"
    port: Optional[str]
    message: str

    @property
    def esptool_before(self) -> str:
        """What to tell esptool. Decided here once: the CLI, the PlatformIO hook
        and the HIL script each used to derive it from the fields above."""
        return "no_reset" if self.entered else "default_reset"


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
                           auth: tuple[str, str] = ("admin", "retimesh"), log: Log = _quiet,
                           ports_fn=list_ports, probe=probe_console, open_console=Console.open,
                           request_http=request_bootloader_http, probe_rom=None,
                           esptool_cmd: Optional[list[str]] = None,
                           sleep=time.sleep, clock=time.monotonic, reappear_timeout: float = 8.0,
                           port_hint: str = "--port") -> HandOff:
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
        return HandOff(False, "none", port, f"{port} is not present; nothing to hand off")
    if chosen is None:
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
                log(f"this board cannot enter its downloader from software: {kv.get('text')}")
                return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                               "leaving the reset to esptool (bridge DTR/RTS)")
            log(f"console refused ({status} {kv}); trying HTTP")
        else:
            if con:
                con.close()
            log(f"no RetiMesh console on {chosen.device}")
            # Silent is what a ROM downloader sounds like too — a node left
            # there by an earlier attempt, or by somebody's BOOT button.
            if probe_rom(chosen.device):
                log("a ROM downloader is already answering on the port")
                return HandOff(True, "downloader", chosen.device, "a ROM downloader is already on the port")

    # 2. HTTP, when a URL is known.
    if node_url_text:
        ok, msg, delay_ms = request_http(node_url_text, auth)
        log(f"HTTP bootloader request: {msg}")
        if ok:
            target = chosen or wait_for_port(lambda ps: select_port(ps), reappear_timeout, ports_fn, sleep, clock)
            if target:
                return _await_downloader(target, "http", log, ports_fn, probe, probe_rom, sleep, clock,
                                         reappear_timeout, delay_ms=delay_ms)
            return HandOff(True, "http", None, "downloader requested but no serial port appeared")

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
    if port.kind == "usb_serial_jtag":
        log(f"watching {port.device} for the reset")
        gone = wait_for_port(lambda ps: None if select_port(ps, device=port.device) else port,
                             window, ports_fn, sleep, clock, interval=0.1)
        if gone is None:
            if probe(port.device, timeout=1.0):
                return HandOff(False, "auto_reset_dtr_rts", port.device,
                               "the console still answers after the request, so the node did not reset; "
                               "leaving the reset to esptool")
            log("the reset gap was not seen and the console is silent; asking esptool")
            return _confirm_downloader(port.device, method, log, probe_rom)
        log(f"{port.device} went away; waiting for it to come back")
    else:
        sleep(window)
        log(f"checking that the console on {port.device} has gone quiet")
        if probe(port.device, timeout=1.0):
            return HandOff(False, "auto_reset_dtr_rts", port.device,
                           "the console still answers after the request, so the node did not reset; "
                           "leaving the reset to esptool")
    back = wait_for_port(lambda ps: select_port(ps, device=port.device), timeout, ports_fn, sleep, clock)
    if back is None:
        # It may have come back under a new name (ttyACM0 -> ttyACM1 on a
        # busy host); accept a single port of the same kind.
        back = wait_for_port(lambda ps: _same_kind(ps, port), 2.0, ports_fn, sleep, clock)
    if back is None:
        return HandOff(False, method, port.device,
                       f"{port.device} did not reappear within {timeout:.0f} s after the bootloader request; "
                       "hold BOOT, press RST, then retry")
    return _confirm_downloader(back.device, method, log, probe_rom)


def _confirm_downloader(device: str, method: str, log: Log, probe_rom) -> HandOff:
    if probe_rom(device):
        return HandOff(True, method, device, "downloader confirmed by esptool")
    log(f"no downloader answers on {device}; leaving the reset to esptool")
    return HandOff(False, "auto_reset_dtr_rts", device,
                   "the port is back but no ROM downloader answers on it; leaving the reset to esptool")


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
    from the downloader (ttyACM1 -> ttyACM0 once the busy host lets go), so a
    single ESP-looking port is accepted when the named one is absent."""
    deadline = clock() + timeout
    announced = None
    while clock() < deadline:
        ports = ports_fn()
        target = select_port(ports, device=port) if port else None
        if target is None:
            candidates = esp_candidates(ports)
            if len(candidates) == 1:
                target = candidates[0]
        if target:
            if target.device != announced:
                log(f"asking VERSION on {target.device}")
                announced = target.device
            info = probe(target.device, timeout=1.5)
            if info:
                return info
        sleep(1.0)
    return None
