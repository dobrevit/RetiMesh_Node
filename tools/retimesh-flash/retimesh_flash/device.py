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
# What to do when the tool has run out of ways in: the ROM's own strapping.
RECOVERY = "hold BOOT, press RST, then retry"

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
        (1C:DB:D4:82:14:54) of one chip compare equal. None unless the serial
        is a MAC: a port with no serial, or one whose serial is something
        else, identifies no chip and must not be taken for one."""
        return mac_node_id(self.serial)

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
        # A MAC names the chip on either of its faces (Port.node_id); any
        # other serial is matched as written.
        want = mac_node_id(serial)
        hits = [p for p in ports if p.serial == serial or (want is not None and p.node_id == want)]
        return hits[0] if len(hits) == 1 else None
    candidates = esp_candidates(ports)
    return candidates[0] if len(candidates) == 1 else None


def mac_node_id(text: Optional[str]) -> Optional[str]:
    """Twelve hex digits, colons dropped, upper-cased — or None."""
    if not text:
        return None
    t = text.replace(":", "").upper()
    return t if re.fullmatch(r"[0-9A-F]{12}", t) else None


def node_id_from_path(device: str) -> Optional[str]:
    """The MAC a /dev/serial/by-id name carries, colons dropped and upper-cased,
    for either face of the chip: usb-RetiMesh_RetiMesh_Node_1CDBD4821454-if00
    or usb-Espressif_USB_JTAG_serial_debug_unit_1C:DB:D4:82:14:54-if00."""
    m = re.search(r"(?:RetiMesh_Node_|serial_debug_unit_)([0-9A-Fa-f:]{12,17})-if", device)
    return mac_node_id(m.group(1)) if m else None


def _link_url(second_octet: int, node_id: Optional[str]) -> Optional[str]:
    """10.<second>.<n>.1 with <n> the last octet of the MAC: the firmware's
    addressing rule (LocalLinkState.h, usbNodeAddress / pppNodeAddress),
    written here a second time because this package cannot include that
    header; the tests hold both to the same vector. None for anything that
    is not a MAC, rather than an address aimed at the wrong subnet."""
    node_id = mac_node_id(node_id)
    return node_url(f"10.{second_octet}.{int(node_id[-2:], 16)}.1") if node_id else None


def usb_node_url(node_id: Optional[str]) -> Optional[str]:
    """Where a node answers over its USB link: 10.64.<n>.1."""
    return _link_url(64, node_id)


def ppp_node_url(node_id: Optional[str]) -> Optional[str]:
    """Where a node asks to answer over PPP: 10.65.<n>.1 — asks, because
    the node is the PPP client and the host's pppd assigns; the pppd
    command the tool prints is what makes the two agree."""
    return _link_url(65, node_id)


def touch_1200(device: str, sleep=time.sleep) -> None:
    """The 1200-baud touch: opening the composite device's ACM port at 1200
    baud with DTR and RTS low makes the node restart into the ROM downloader
    (the firmware takes the line-coding event as a bootloader request). The
    port may vanish under that open, which pyserial reports as an error that
    there means success — so only the second open's failure is swallowed. The
    first open failing means the port cannot be used at all (held by another
    program, no permission, gone), and that is raised: an earlier version
    swallowed it too and then waited three minutes for a downloader nobody
    had asked for."""
    import serial
    # The node reports a line coding only when it changes, so a port left at
    # 1200 by an earlier touch is opened at the console's speed first: the
    # second touch is then a change too.
    for baud in (CONSOLE_BAUD, 1200):
        ser = serial.Serial()
        ser.port = device
        ser.baudrate = baud
        ser.dtr = False
        ser.rts = False
        try:
            ser.open()
            sleep(0.2)
            ser.close()
        except Exception:
            if baud == CONSOLE_BAUD:
                raise


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
    # The board moved to the top of the document, where a node's make and model
    # belongs; older firmware answers with it under "power" and is still read.
    board = doc.get("board") or (doc.get("power") or {}).get("board") or "?"
    return NodeInfo(doc["firmware"], doc.get("version", "?"), board, base_url)


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
# PPP over the bridge UART
#
# On the CP2102/CH9102 boards the host can run pppd on the node's serial
# port and reach it as a network interface (docs/local-link.md). While it
# does, the console on that port is unreachable — PPP owns it — and so is
# esptool: pppd holds the tty with the PPP line discipline on it, and a
# second opener would inject bytes into the frames. Everything here is
# about knowing that before touching the port. pppd needs root on most
# distributions, so nothing here runs it: the commands are printed.
# ---------------------------------------------------------------------------
PPP_DEFAULT_BAUD = 115200
# The pppd invocation the firmware expects (PppUart.h): the node is the PPP
# client, pppd the server — `local` because a USB bridge has no carrier to
# watch, and the LCP echoes because they are how the node tells a dead host
# from an idle one (it hands its port back to the console after 30 s of
# silence) and how pppd notices the node restarting into its downloader.
PPPD_OPTIONS = ["noauth", "local", "nodetach", "lcp-echo-interval", "5", "lcp-echo-failure", "4"]


@dataclass
class PppLink:
    ifname: str                       # ppp0
    node_ip: str                      # the far end: where the node answers
    host_ip: Optional[str]            # our end

    @property
    def url(self) -> str:
        return f"http://{self.node_ip}"


@dataclass
class Pppd:
    pid: int
    port: Optional[str]               # the tty on its command line
    baud: Optional[int]
    argv: list


def _ip_routes() -> list:
    """`ip -4 route show`, as lines; nothing where there is no `ip`."""
    import subprocess
    try:
        rc = subprocess.run(["ip", "-4", "route", "show"], capture_output=True, text=True, timeout=5)
    except Exception:
        return []
    return rc.stdout.splitlines() if rc.returncode == 0 else []


def ppp_links(routes: Optional[Iterable[str]] = None) -> list:
    """The point-to-point links this host has up, from the routing table
    (`ip -4 route show`, injectable): a host route to the far end through a
    pppN device, with our own address as its source. The node is the far end."""
    out = []
    for line in (_ip_routes() if routes is None else routes):
        m = re.match(r"^(\d+\.\d+\.\d+\.\d+)\s+dev\s+(ppp\d+)\b(?:.*?\bsrc\s+(\d+\.\d+\.\d+\.\d+))?", line.strip())
        if m:
            out.append(PppLink(m.group(2), m.group(1), m.group(3)))
    return out


def _proc_cmdlines() -> list:
    """(pid, argv) for every process /proc lets us read; nothing elsewhere."""
    out = []
    try:
        pids = [p for p in os.listdir("/proc") if p.isdigit()]
    except Exception:
        return out
    for pid in pids:
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv = [a.decode("utf-8", "replace") for a in f.read().split(b"\0") if a]
        except Exception:
            continue
        if argv:
            out.append((int(pid), argv))
    return out


def find_pppd(port: Optional[str] = None, cmdlines: Optional[Iterable] = None) -> list:
    """Every pppd on this host, or the ones holding `port` — from the
    command lines /proc exposes to everyone (injectable), since the
    process itself is root's and its open files are not ours to list. The
    port is the first /dev argument, the speed the first bare number."""
    out = []
    for pid, argv in (_proc_cmdlines() if cmdlines is None else cmdlines):
        if not argv or os.path.basename(argv[0]) != "pppd":
            continue
        dev = next((a for a in argv[1:] if a.startswith("/dev/")), None)
        baud = next((int(a) for a in argv[1:] if a.isdigit()), None)
        if port and (dev is None or not same_device(dev, port)):
            continue
        out.append(Pppd(pid, dev, baud, list(argv)))
    return out


def ppp_addresses(links_data: Iterable[dict]) -> Optional[tuple]:
    """(node, host, baud, enabled) for the PPP link, from the console's LINKS
    reply: the addresses the node asks for, which are what pppd is told to
    assign. None when the node has no PPP link this build can run."""
    for d in links_data:
        if d.get("link") == "ppp0" and d.get("firmware") == "yes" and d.get("asks") and d.get("peer"):
            return d["asks"], d["peer"], _int(d.get("baud"), PPP_DEFAULT_BAUD), d.get("enabled") == "yes"
    return None


def pppd_command(port: str, node_ip: str, host_ip: str, baud: int = PPP_DEFAULT_BAUD) -> list:
    """The pppd command line for one node on one port, ready to print. The
    host's address comes first in pppd's local:remote pair."""
    return ["pppd", port, str(baud), *PPPD_OPTIONS, f"{host_ip}:{node_ip}"]


def shell_words(argv: Iterable[str]) -> str:
    import shlex
    return " ".join(shlex.quote(a) for a in argv)


# How long the host may take to let go of the port once the node has been
# told to restart: pppd sees the node stop answering its LCP echoes — four
# five-second echoes — or the node's own Terminate-Request as it goes down,
# whichever first, and exits. `persist` would keep it dialling for ever,
# which is why the documented command line does not carry it.
_PPP_RELEASE_S = 40.0


def _hand_off_over_ppp(port: Port, pppd: Pppd, link: Optional[PppLink], auth, log: Log, request_http,
                       pppd_fn, probe_rom, sleep, clock) -> "HandOff":
    """pppd holds the port. The console is not reachable there, but the node
    is — over the link pppd made — so the bootloader is asked for over HTTP,
    and then the port is waited for: esptool cannot have it until pppd has
    let it go, and only root can make pppd do that."""
    stop = f"sudo kill {pppd.pid}"
    if link is None:
        return HandOff(False, "none", port.device,
                       f"pppd (pid {pppd.pid}) holds {port.device} but no ppp interface has a route up; "
                       f"stop it ({stop}) and retry, or wait for the link to come up", node_id=port.node_id)
    log(f"pppd (pid {pppd.pid}) holds {port.device}; the node is at {link.url} over {link.ifname}")
    ok, msg, delay_ms = request_http(link.url, auth)
    log(f"HTTP bootloader request over {link.ifname}: {msg}")
    if not ok:
        # A 501 is a classic ESP32 that esptool must reset itself — which
        # it cannot do while pppd has the port. Nothing here may kill a
        # root process; the operator is told exactly what to run.
        return HandOff(False, "none", port.device,
                       f"{msg}; stop pppd first ({stop}), then retry — esptool needs the port",
                       node_id=port.node_id)
    log(f"waiting for pppd to release {port.device} (up to {_PPP_RELEASE_S:.0f} s)")
    deadline = clock() + delay_ms / 1000.0 + _PPP_RELEASE_S
    while pppd_fn(port.device) and clock() < deadline:
        sleep(1.0)
    if pppd_fn(port.device):
        return HandOff(False, "http", port.device,
                       f"the node accepted the request but pppd (pid {pppd.pid}) still holds {port.device}; "
                       f"stop it ({stop}) and retry with the reset left to esptool", node_id=port.node_id)
    log(f"pppd has let go of {port.device}")
    return _confirm_downloader(port, "http", log, probe_rom)


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


def downloader_present(device: str, timeout: float = 15.0, esptool_cmd: Optional[list[str]] = None,
                       log: Log = _quiet) -> bool:
    """Whether a ROM downloader answers on the port, asked the one way that is
    definitive: esptool's own sync, with no reset before or after. A silent
    console cannot tell a downloader from a dead node, and the difference
    decides what esptool must be told — a DTR/RTS reset performed on a chip
    already in its downloader re-enumerates the port under esptool's own
    open, which is how a node left in the downloader by one failed flash
    failed the next one too.

    A tool that cannot run at all is not an answer about the port, and it is
    said so: an esptool missing from the interpreter that was handed in
    returns the same "no" as a silent port, and that "no" once sent a caller
    down the bridge-reset path and left a node sitting in its ROM. The
    verdict stays the cautious one either way — nothing here can confirm a
    downloader it never reached — but the log says which of the two it was.
    """
    import subprocess
    cmd = [*(esptool_cmd or DEFAULT_ESPTOOL_CMD),
           *esptool_args(None, device, "no_reset", "no_reset", "chip_id")]
    try:
        rc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError as exc:
        log(f"esptool could not be run ({exc}); the ROM downloader cannot be confirmed")
        return False
    except Exception as exc:
        log(f"esptool did not finish on {device} ({exc}); the ROM downloader cannot be confirmed")
        return False
    if rc.returncode != 0 and "No module named" in (rc.stderr or ""):
        log(f"esptool is not installed for {cmd[0]}; the ROM downloader cannot be confirmed")
        return False
    return rc.returncode == 0


# ---------------------------------------------------------------------------
# The hand-off
# ---------------------------------------------------------------------------
@dataclass
class HandOff:
    entered: bool                     # the ROM downloader is known to be up on `port`
    method: str                       # "console", "touch", "http", "downloader", "auto_reset_dtr_rts", "none"
    port: Optional[str]               # where to point esptool; None when there is nowhere
    message: str
    node_id: Optional[str] = None     # the chip's MAC where its port reported one: what wait_for_application follows

    @property
    def esptool_before(self) -> str:
        """What to tell esptool. Decided here once: the CLI, the PlatformIO hook
        and the HIL script each used to derive it from the fields above.

        esptool's own reset at connect, always — even on a downloader that is
        already up. On the S3's serial-JTAG unit a ROM entered from software
        stays in the downloader through esptool's closing hard reset unless
        esptool's connect-time sequence ran first (measured; the bench spent
        an afternoon on it), and on a bridge that sequence re-enters the ROM
        through IO0, which is harmless. The one port esptool must not reset,
        the composite device, is never the port a downloader answers on: the
        ROM comes up on the serial-JTAG unit, and the hand-off points esptool
        there. An earlier version carried a no_reset case for a port that
        could not drive a reset; no port that reaches here is one."""
        return "default_reset"


def wait_for_port(predicate: Callable[[list[Port]], Optional[Port]], timeout: float,
                  ports_fn=list_ports, sleep=time.sleep, clock=time.monotonic,
                  interval: float = 0.25, progress: Optional[Callable[[float], None]] = None,
                  progress_every: float = 15.0) -> Optional[Port]:
    """Poll the ports until `predicate` picks one or `timeout` runs out.
    `progress`, if given, hears the seconds waited every `progress_every`."""
    started = clock()
    deadline = started + timeout
    told = 0
    while True:
        hit = predicate(ports_fn())
        now = clock()
        if hit or now >= deadline:
            return hit
        if progress and now - started >= (told + 1) * progress_every:
            told += 1
            progress(now - started)
        sleep(interval)


def hand_off_to_bootloader(port: Optional[str] = None, node_url_text: Optional[str] = None,
                           auth: tuple[str, str] = DEFAULT_ADMIN, log: Log = _quiet,
                           ports_fn=list_ports, probe=probe_console, open_console=Console.open,
                           request_http=request_bootloader_http, probe_rom=None,
                           esptool_cmd: Optional[list[str]] = None,
                           sleep=time.sleep, clock=time.monotonic, reappear_timeout: float = 8.0,
                           port_hint: str = "--port", touch=touch_1200,
                           pppd_fn=find_pppd, ppp_links_fn=ppp_links) -> HandOff:
    """Console first, HTTP second, esptool's own reset last.

    Returns where esptool should point and whether the downloader is known to
    be up. `entered` is False when the board is left to esptool's DTR/RTS
    reset, which is the normal outcome on a bridge or USB-Serial/JTAG port and
    not a failure."""
    if probe_rom is None:
        probe_rom = lambda dev: downloader_present(dev, esptool_cmd=esptool_cmd, log=log)  # noqa: E731
    ports = ports_fn()
    chosen = select_port(ports, device=port)
    # A port pppd holds is not opened, and not left to esptool either: the
    # node is asked over the link pppd made instead (docs/local-link.md).
    # Before anything else, because opening the port would write into the
    # PPP stream and the console on it would not answer anyway.
    if chosen:
        holders = pppd_fn(chosen.device)
        if holders:
            links = ppp_links_fn()
            link = links[0] if len(links) == 1 else None
            return _hand_off_over_ppp(chosen, holders[0], link, auth, log, request_http,
                                      pppd_fn, probe_rom, sleep, clock)
    if port and chosen is None:
        # The named port is not there. If it was the composite device and the
        # chip is already in its ROM — an earlier attempt got it there and
        # then lost track — its serial-JTAG downloader is on the bus under
        # the same MAC, which the by-id name carries.
        node_id = node_id_from_path(port)
        rom = _port_of(ports, "usb_serial_jtag", node_id) if node_id else None
        if rom and probe_rom(rom.device):
            log(f"{port} is gone, but the chip's serial-JTAG downloader answers on {rom.device}")
            return HandOff(True, "downloader", rom.device, "the downloader is already on the port", node_id=rom.node_id)
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
        # Whether the console, if there is one, can get the chip into its
        # downloader: it may be absent (switched off, or no RetiMesh firmware)
        # or answer 501 (a firmware that presents the composite device but has
        # not learnt to enter from its console).
        cannot_enter = info is None
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
            cannot_enter = status == "ERR" and kv.get("code") == 501
            if cannot_enter and chosen.kind != "retimesh_composite":
                log(f"this board cannot enter its downloader from software: {kv.get('text')}")
                return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                               "leaving the reset to esptool (bridge DTR/RTS)", node_id=chosen.node_id)
            if not cannot_enter:
                log(f"console refused ({status} {kv}); trying HTTP")
        else:
            if con:
                con.close()
            log(f"no RetiMesh console on {chosen.device}")
        if cannot_enter and chosen.kind == "retimesh_composite":
            # The composite device's own touch needs no console.
            log("using the 1200-baud touch")
            try:
                touch(chosen.device)
            except Exception as exc:
                return HandOff(False, "none", chosen.device,
                               f"could not open {chosen.device} for the 1200-baud touch ({exc}); {RECOVERY}",
                               node_id=chosen.node_id)
            return _await_downloader(chosen, "touch", log, ports_fn, probe, probe_rom, sleep, clock,
                                     reappear_timeout)
        if info is None and probe_rom(chosen.device):
            # Silent is what a ROM downloader sounds like too — a node left
            # there by an earlier attempt, or by somebody's BOOT button.
            log("a ROM downloader is already answering on the port")
            return HandOff(True, "downloader", chosen.device, "a ROM downloader is already on the port",
                           node_id=chosen.node_id)

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
            # field promises a port the downloader is known to be up on.
            return HandOff(False, "http", None, f"downloader requested but no serial port appeared; {RECOVERY}")

    # 3. Nothing worked, or nothing needed to: esptool resets what it can.
    if chosen and chosen.auto_reset:
        return HandOff(False, "auto_reset_dtr_rts", chosen.device,
                       f"leaving the reset to esptool on {chosen.device} ({chosen.kind})", node_id=chosen.node_id)
    return HandOff(False, "none", chosen.device if chosen else None,
                   f"could not reach a RetiMesh node; if the upload fails, {RECOVERY}",
                   node_id=chosen.node_id if chosen else None)


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

# How long the application may take to answer after a flash. A bridge board's
# port belongs to the bridge and never moves, so silence past a few seconds is
# a real failure. A native-USB chip comes back as a new device on the bus, and
# a host that was slow to notice it leave is as slow to notice it return — the
# same hub behaviour _COMPOSITE_DOWNLOADER_S is set by, so this is derived
# from it and the two cannot drift apart. One rule, because the CLI, the
# PlatformIO hook and the HIL script each carried a number of their own (20,
# 90 and 40 s) and two of the three were under what one bench hub takes to
# report a device arriving at all.
_APPLICATION_BACK_BRIDGE_S = 20.0
_APPLICATION_BACK_NATIVE_S = _COMPOSITE_DOWNLOADER_S


def application_wait_s(node_id: Optional[str] = None, port: Optional[str] = None,
                       ports_fn=list_ports) -> float:
    """How long to wait for the application, for this chip. See the constants
    above; a chip known by its MAC is a native-USB chip, and so is a port that
    is one of its two faces."""
    if mac_node_id(node_id) or (port and node_id_from_path(port)):
        return _APPLICATION_BACK_NATIVE_S
    hit = select_port(ports_fn(), device=port) if port else None
    if hit and hit.kind in ("retimesh_composite", "usb_serial_jtag"):
        return _APPLICATION_BACK_NATIVE_S
    return _APPLICATION_BACK_BRIDGE_S


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
        # same chip by the MAC both report — or, when this port reported no
        # MAC, by being new on the bus since the request. A serial-JTAG unit
        # that was already there is somebody else's chip.
        before = {p.device for p in ports_fn() if p.kind == "usb_serial_jtag"}
        def still_here(ps):
            # By MAC when known; by path otherwise — with two nameless
            # composite devices on the bench, "the one composite device" is
            # nobody, and the port would have counted as gone before it went.
            if port.node_id:
                return _port_of(ps, "retimesh_composite", port.node_id)
            return next((p for p in ps if p.kind == "retimesh_composite" and same_device(p.device, port.device)), None)
        def downloader(ps):
            if port.node_id:
                return _port_of(ps, "usb_serial_jtag", port.node_id)
            fresh = [p for p in ps if p.kind == "usb_serial_jtag" and p.device not in before]
            return fresh[0] if len(fresh) == 1 else None
        log(f"watching for {port.device} to go and the serial-JTAG downloader to come")
        gone = wait_for_port(lambda ps: None if still_here(ps) else port,
                             window, ports_fn, sleep, clock, interval=0.1)
        if gone is None and probe(port.device, timeout=1.0):
            return _not_reset(port)
        if gone is None:
            log("the node has stopped answering; its port will drop when the host notices — "
                "at once on a root port, up to a minute behind some hubs")
        back = wait_for_port(downloader, _COMPOSITE_DOWNLOADER_S, ports_fn, sleep, clock,
                             progress=lambda waited: log(f"still waiting for the serial-JTAG downloader ({waited:.0f} s)"))
        if back is None:
            return HandOff(False, method, None,
                           f"the serial-JTAG downloader did not appear within {_COMPOSITE_DOWNLOADER_S:.0f} s "
                           f"after the request; {RECOVERY}")
        log(f"the downloader is on {back.device}")
        return _confirm_downloader(back, method, log, probe_rom)
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
            return _not_reset(port)
        log("the console is silent; asking esptool")
        return _confirm_downloader(port, method, log, probe_rom)
    log(f"{port.device} went away; waiting for it to come back")
    back = wait_for_port(lambda ps: select_port(ps, device=port.device), timeout, ports_fn, sleep, clock)
    if back is None:
        # It may have come back under a new name (ttyACM0 -> ttyACM1 on a
        # busy host); accept a single port of the same kind.
        back = wait_for_port(lambda ps: _port_of(ps, port.kind, None), 2.0, ports_fn, sleep, clock)
    if back is None:
        # No port: the one that was there has gone and nothing came back in
        # its place. Naming the path it used to be on would read as somewhere
        # esptool could open — the composite branch above answers None for the
        # same situation, and both mean the same thing.
        return HandOff(False, method, None,
                       f"{port.device} did not reappear within {timeout:.0f} s after the bootloader request; {RECOVERY}")
    return _confirm_downloader(back, method, log, probe_rom)


def _not_reset(port: Port) -> HandOff:
    """The console still answers after the request: the node did not reset.
    What follows is whatever the port can do — esptool's own reset where the
    lines reach the chip, the buttons where they do not (the composite device)."""
    why = "the console still answers after the request, so the node did not reset; "
    if port.auto_reset:
        return HandOff(False, "auto_reset_dtr_rts", port.device, why + "leaving the reset to esptool", node_id=port.node_id)
    return HandOff(False, "none", port.device, why + RECOVERY, node_id=port.node_id)


def _confirm_downloader(port: Port, method: str, log: Log, probe_rom) -> HandOff:
    if probe_rom(port.device):
        return HandOff(True, method, port.device, "downloader confirmed by esptool", node_id=port.node_id)
    log(f"no downloader answers on {port.device}; leaving the reset to esptool")
    return HandOff(False, "auto_reset_dtr_rts", port.device,
                   "the port is back but no ROM downloader answers on it; leaving the reset to esptool",
                   node_id=port.node_id)


def _port_of(ports: list[Port], kind: str, node_id: Optional[str]) -> Optional[Port]:
    """The one port of this kind belonging to this chip — or, when the chip's
    MAC is unknown, the one port of this kind at all."""
    hits = [p for p in ports if p.kind == kind and (node_id is None or p.node_id == node_id)]
    return hits[0] if len(hits) == 1 else None


def wait_for_application(port: Optional[str], timeout: Optional[float] = None, log: Log = _quiet,
                         probe=probe_console, ports_fn=list_ports, sleep=time.sleep,
                         clock=time.monotonic, node_id: Optional[str] = None) -> Optional[NodeInfo]:
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
    if timeout is None:
        timeout = application_wait_s(node_id, port, ports_fn)
    started = clock()
    deadline = started + timeout
    announced = set()
    told = 0
    # The chip that was flashed is known by its MAC — the hand-off's
    # (HandOff.node_id, from the port the downloader answered on) or the one
    # in a by-id port name — on a native-USB board; when its port comes back
    # under another face (the composite device after the serial-JTAG
    # downloader), only that chip is asked. Without it, a bench with several
    # nodes answered with whichever was first, and once reported a soak node
    # as the application coming back. The downloader's pyserial name, which
    # is what the callers hold after a hand-off, carries no MAC: an earlier
    # version looked for one there and never found it.
    node_id = mac_node_id(node_id) or (node_id_from_path(port) if port else None)
    while clock() < deadline:
        ports = ports_fn()
        named = select_port(ports, device=port) if port else None
        if node_id:
            # This chip by its MAC — and, failing that, any candidate that
            # names no chip at all (a port without a usable serial), which
            # may be it; never one that names another chip.
            # A chip known by its MAC is a native-USB chip; it comes back as
            # the composite device or the serial-JTAG unit, never as a
            # bridge, so a bridge with no usable serial is not a candidate.
            targets = [p for p in ports if p.node_id == node_id] or \
                      [p for p in esp_candidates(ports)
                       if p.node_id is None and p.kind in ("retimesh_composite", "usb_serial_jtag")]
            # The hinted port is asked as well, but last and never instead:
            # a device node can sit in the host's list for a minute after the
            # device behind it left (the hub above), and an earlier version
            # asked that one port for the whole timeout while the chip was
            # answering under another name.
            if named and named not in targets:
                targets = targets + [named]
        elif named:
            targets = [named]
        else:
            targets = esp_candidates(ports)
        waited = clock() - started
        if waited >= (told + 1) * 15.0:
            told = int(waited // 15.0)
            log(f"still waiting for the application ({waited:.0f} s)")
        for target in targets:
            if target.device not in announced:
                log(f"asking VERSION on {target.device}")
                announced.add(target.device)
            info = probe(target.device, timeout=1.5)
            if info:
                return info
        sleep(1.0)
    return None
