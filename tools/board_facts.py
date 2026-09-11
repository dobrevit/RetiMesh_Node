#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node.
#
# RetiMesh Node is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# RetiMesh Node is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

"""What a board actually has, worked out the way the compiler works it out.

`boards.json` is a catalogue of capability, and a catalogue is only worth
having if something can prove it true. This module is that something: it
answers the same questions the build answers, from the same sources, so a
claim in the registry can be checked rather than trusted.

Why this is not a one-liner
---------------------------
The premise of the capability catalogue is that four prose fields cannot say
what a board has. The reason they cannot is that the firmware does not keep
that knowledge in one place either — a single fact can come from any of three
sources, and they have an order:

  1. `#define` in `src/boards/<board>.h`. The board's own statement, where
     most of these facts are written, and — because these are unguarded
     defines rather than `#ifndef` — the one that wins outright. A `-D` is a
     define at the top of the translation unit and a later unguarded `#define`
     redefines it: `gcc -DHAS_SD=0` over `#define HAS_SD 1` prints 1.
  2. `-D` in the env's `build_flags` in `platformio.ini`, or in the board
     manifest's own `extra_flags`. Beats only what follows. `HAS_LVGL_UI`
     lives here and nowhere else, on the three envs with a graphical shell.
  3. `#ifndef X / #define X <default>` in `src/Config.h`. The fallback every
     board inherits by saying nothing.

Read only the first and a board looks featureless; read only the second and
`HAS_LVGL_UI` disappears; read only the third and every board looks identical.
The resolver below reads all three, in that order, which is the only reading
that matches what gets compiled.

Two traps this exists to avoid
------------------------------
**Interpolation.** PlatformIO expands `${section.option}` in its own syntax —
a dot, not configparser's colon — and the PSRAM flag reaches four boards that
way, through `${esp32s3_psram.build_flags}`. A resolver that reads an env's
raw `build_flags` sees the literal `${...}` and reports those boards as having
no PSRAM. They have 2 to 8 MB of it.

**The board manifest.** `BOARD_HAS_PSRAM` can also arrive from PlatformIO's own
board file rather than from this repository at all: `ttgo-t-beam.json` carries
it in `build.extra_flags`, so the T-Beam has PSRAM while nothing in
`platformio.ini` says so. Both places have to be read or the answer is wrong
for that board and confidently so.

A silent header is not an absent part
-------------------------------------
`resolve_define` returns `Config.h`'s default when neither the env nor the
header speaks, because that is what the compiler does. That makes the answer
right about the *build* and potentially wrong about the *hardware*: a board
with no panel inherits `DISPLAY_WIDTH 128` exactly as a 128-wide board does.
Callers that turn these into catalogue claims must gate on the flag that says
the part exists — `HAS_DISPLAY` before any geometry — rather than recording a
default as a measurement.
"""

from __future__ import annotations

import configparser
import json
import os
import re
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent

# Where PlatformIO keeps the board manifests for this checkout's platform. The
# core-3 tree, not the default one: the sibling RNode_Firmware mirror owns
# ~/.platformio, and the two platforms uninstall each other (project-brief).
_PLATFORM_DIRS = [
    Path(os.environ.get("PLATFORMIO_PLATFORMS_DIR",
                        Path.home() / ".platformio" / "platforms-core3")),
    Path.home() / ".platformio" / "platforms",
]

_INTERP = re.compile(r"\$\{([A-Za-z0-9_:\-]+)\.([A-Za-z0-9_.]+)\}")


class Ini:
    """`platformio.ini`, with PlatformIO's interpolation and `extends`."""

    def __init__(self, path: Path = None):
        self._cp = configparser.ConfigParser()
        self._cp.read(path or ROOT / "platformio.ini")

    def sections(self):
        return self._cp.sections()

    def has_env(self, env: str) -> bool:
        return self._cp.has_section(f"env:{env}")

    def raw(self, section: str, key: str) -> Optional[str]:
        # raw=True throughout: configparser's own `%(...)s` interpolation is
        # not PlatformIO's and would raise on values it does not recognise.
        if self._cp.has_section(section) and self._cp.has_option(section, key):
            return self._cp.get(section, key, raw=True)
        return None

    def option(self, env: str, key: str) -> Optional[str]:
        """One option for an env, following `extends` when it does not have it."""
        section = f"env:{env}"
        seen = set()
        while section and section not in seen:
            seen.add(section)
            v = self.raw(section, key)
            if v is not None:
                return v
            ext = self.raw(section, "extends")
            section = ext if ext else None
        return None

    def expand(self, section: str, key: str, _depth: int = 0) -> str:
        """A value with `${section.option}` resolved and `extends` prepended.

        Depth-limited rather than cycle-tracked: the file is small, a cycle
        would be a mistake rather than a design, and a bounded walk says so by
        producing a short answer instead of hanging a CI job.
        """
        if _depth > 8:
            return ""
        parts = []
        ext = self.raw(section, "extends")
        if ext:
            parts.append(self.expand(ext, key, _depth + 1))
        value = self.raw(section, key) or ""
        for m in _INTERP.finditer(value):
            sec, opt = m.group(1), m.group(2)
            if self._cp.has_section(sec):
                value = value.replace(m.group(0), self.expand(sec, opt, _depth + 1))
        parts.append(value)
        return "\n".join(p for p in parts if p)

    def build_flags(self, env: str) -> str:
        return self.expand(f"env:{env}", "build_flags")


def manifest(board: str) -> Optional[dict]:
    """PlatformIO's own board file, which can carry defines of its own.

    The vendored copy under `boards/` first and the installed platform second,
    which is the order `make_manifest.py:board_json` already used and for the
    same reason: CI's tools job installs neither PlatformIO nor the platform,
    so a resolver that could only read the installed tree failed every board
    for want of a manifest — reporting the correct catalogue as wrong, which
    is the exact inverse of what the gate is for. See boards/README.md.
    """
    for p in manifest_paths(board):
        if p.exists():
            return json.loads(p.read_text())
    return None


def manifest_paths(board: str) -> list:
    """Where a manifest may live, vendored copy first. Exposed so the tests
    can compare the two when both exist."""
    return [ROOT / "boards" / f"{board}.json"] + [
        base / "espressif32" / "boards" / f"{board}.json" for base in _PLATFORM_DIRS]


def _defines(text) -> dict:
    """`-DNAME` / `-DNAME=value` out of a build_flags blob.

    Accepts a list as well as a string: PlatformIO's board manifests write
    `build.extra_flags` either way — `ttgo-t-beam.json` as a string,
    `esp32-s3-devkitc-1.json` as a list — and a resolver that assumed one
    shape crashed on the other.
    """
    if isinstance(text, (list, tuple)):
        text = " ".join(str(t) for t in text)
    out = {}
    for m in re.finditer(r"-D([A-Za-z_][A-Za-z0-9_]*)(?:=([^\s]+))?", text):
        out[m.group(1)] = m.group(2) if m.group(2) is not None else "1"
    return out


class Board:
    """One env, and what its build actually defines."""

    def __init__(self, env: str, ini: Ini = None, config_h: str = None):
        self.env = env
        self.ini = ini or Ini()
        self._config_h = config_h if config_h is not None else (
            ROOT / "src" / "Config.h").read_text()
        self._flags = _defines(self.ini.build_flags(env))
        self._board = self.ini.option(env, "board")
        man = manifest(self._board) if self._board else None
        self._manifest = man or {}
        self._manifest_flags = _defines(
            (self._manifest.get("build", {}) or {}).get("extra_flags", "") or "")
        self._header_text = None

    # -- sources, in the order the compiler sees them ----------------------

    @property
    def header(self) -> Optional[str]:
        """The board header `src/Config.h` dispatches this env's flag to.

        Resolved through the same chain `check_boards.py` validates rather
        than from a table here, so a board that changes header changes this
        answer with it. Envs that pass no `BOARD_*` flag build the chain's
        closing `#else`, which is the T3-S3's header.
        """
        # In file order, because the chain is an #if/#elif and the compiler
        # takes the first arm that matches. Config.h tests BOARD_T3S3_SX1280_PA
        # before BOARD_T3S3_SX1280 deliberately, one being a prefix-alike of
        # the other; iterating the env's flags instead would pick whichever the
        # build_flags happened to list first.
        for flag, header in re.findall(
                r'defined\((BOARD_[A-Z0-9_]+)\)\s*\)?\s*\n\s*#include\s+"boards/([a-z0-9_]+)\.h"',
                self._config_h):
            if flag in self._flags:
                return header
        # The chain's closing #else, read from Config.h rather than written
        # here: an env that passes no BOARD_* flag builds whatever that arm
        # names, and hardcoding it would go quietly wrong if it ever changed.
        m = re.search(r'#else\s*\n\s*#include\s+"boards/([a-z0-9_]+)\.h"', self._config_h)
        return m.group(1) if m else "t3s3"

    def _header_source(self) -> str:
        if self._header_text is None:
            p = ROOT / "src" / "boards" / f"{self.header}.h"
            self._header_text = p.read_text() if p.exists() else ""
        return self._header_text

    def _config_default(self, name: str) -> Optional[str]:
        m = re.search(r"#ifndef\s+" + re.escape(name) + r"\s*\n\s*#define\s+"
                      + re.escape(name) + r"\s+([^\s/]+)", self._config_h)
        return m.group(1) if m else None

    # -- the answer --------------------------------------------------------

    def resolve_define(self, name: str) -> Optional[str]:
        """What the build sees for `name`, or None if nothing defines it.

        The board header first, and that order is not the intuitive one. A
        `-D` is a `#define` at the top of the translation unit, so a later
        *unguarded* `#define` in a header redefines it and wins — with a
        `-Wmacro-redefined` warning, but it wins. Verified rather than assumed:
        `gcc -DHAS_SD=0` over a source containing `#define HAS_SD 1` prints 1.
        Board headers define these names unguarded (`t3s3.h:28` is
        `#define HAS_SD 1`, not an `#ifndef`), so the header is the answer
        wherever it speaks.

        A command-line `-D` therefore beats only `Config.h`'s `#ifndef`
        defaults — which is still how `HAS_LVGL_UI` reaches its three boards,
        since no header defines it. An earlier version of this file had the
        first two the other way round and said so in three places; the order is
        only visible when an env and a header disagree, which none do today,
        and `check_boards.py` now refuses that collision outright so that the
        distinction cannot start mattering silently.
        """
        m = re.search(r"^\s*#define\s+" + re.escape(name) + r"\s+([^\s/]+)",
                      self._header_source(), re.M)
        if m:
            return m.group(1)
        if name in self._flags:
            return self._flags[name]
        if name in self._manifest_flags:
            return self._manifest_flags[name]
        return self._config_default(name)

    def header_defines(self) -> set:
        """Names the board header defines unguarded — the ones a `-D` cannot
        override. `check_boards.py` uses this to refuse the collision."""
        return set(re.findall(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+[^\s/]+",
                              self._header_source(), re.M))

    def env_defines(self) -> dict:
        """What this env puts on the command line, manifest flags included."""
        merged = dict(self._manifest_flags)
        merged.update(self._flags)
        return merged

    def flag(self, name: str) -> bool:
        """A `HAS_*`-style switch as a bool. Absent or `0` are both false."""
        v = self.resolve_define(name)
        return bool(v) and v not in ("0", "false", "False")

    def number(self, name: str) -> Optional[int]:
        v = self.resolve_define(name)
        if v is None:
            return None
        try:
            return int(v, 0)
        except ValueError:
            return None

    # -- facts that come from the ini and the manifest rather than a define -

    @property
    def mcu(self) -> Optional[str]:
        return (self._manifest.get("build", {}) or {}).get("mcu")

    @property
    def psram(self) -> bool:
        """PSRAM reaches a board three ways and all of them count.

        `-DBOARD_HAS_PSRAM` in the env, the same flag arriving through an
        interpolated base section, or the board manifest's own `extra_flags`.
        resolve_define covers the first two; the third is why it also reads
        the manifest.
        """
        return self.flag("BOARD_HAS_PSRAM")

    @property
    def flash_mb(self) -> Optional[int]:
        """The env's stated flash size, else the manifest's.

        `board_upload.flash_size` is the one that matters when present: several
        envs run a 4 MB or 16 MB part on a manifest whose default is 8 MB, and
        the partition table is cut to the env's figure rather than the
        manifest's.

        The fallback is the manifest's `upload.flash_size` and deliberately not
        its `upload.maximum_size`, which is the largest *application image* and
        a different number. They coincide on the four manifests this repository
        uses, so reading the wrong one is right today and wrong on the next
        board: `esp32dev` caps an application at 1 310 720 bytes on a 4 MB
        part, and a catalogue derived from that would claim `flash_mb: 1` and
        then have CI enforce it. `make_manifest.py:120` already reads the right
        field; this now agrees with it.
        """
        for v in (self.ini.option(self.env, "board_upload.flash_size"),
                  (self._manifest.get("upload", {}) or {}).get("flash_size")):
            if v:
                m = re.match(r"(\d+)\s*MB", str(v).strip(), re.I)
                if m:
                    return int(m.group(1))
        return None

    @property
    def partitions(self) -> Optional[str]:
        """The env's table, else the one the manifest hands it.

        `esp32s3-qspi` names none and still has one: the devkit manifest
        carries `build.arduino.partitions`, which `make_manifest.py` documents
        as the reason it reads the built binary table rather than the ini. A
        catalogue that recorded silence there would be saying the board has no
        partition table, which is not true of any board that boots.
        """
        v = self.ini.option(self.env, "board_build.partitions")
        if v:
            return v
        arduino = (self._manifest.get("build", {}) or {}).get("arduino", {}) or {}
        return arduino.get("partitions")


def envs(registry: dict) -> list:
    """The board envs in the registry, in file order, without the notes keys."""
    return [k for k in registry if not k.startswith("_")]


def registry() -> dict:
    return json.loads((ROOT / "boards.json").read_text())


# ---------------------------------------------------------------------------
# The catalogue's own rules
# ---------------------------------------------------------------------------
# Written here rather than in check_boards.py because two callers need the
# same answer and neither may drift from the other: the CI gate runs it to
# fail a build, and tools/tests/test_board_catalogue.py runs it so the rule is
# exercised by `unittest discover` as well. One rule, asked twice.

_PERIPHERALS = ("gnss", "sd", "pmu", "battery_adc", "imu",
                "compass", "environment", "touch", "buzzer")

_PERIPHERAL_FLAG = {
    "gnss": "HAS_GPS", "sd": "HAS_SD", "pmu": "HAS_PMU",
    "battery_adc": "HAS_BATTERY_ADC", "imu": "HAS_IMU",
    "compass": "HAS_COMPASS", "environment": "HAS_ENV",
    "touch": "HAS_TOUCH", "buzzer": "HAS_BUZZER",
}

# The names the catalogue derives an answer from. A `-D` colliding with one of
# these makes a registry entry wrong, so it is refused; a collision on any
# other name is a no-op flag rather than a lie in the catalogue, and is said
# out loud instead of failing a build this change is not the owner of.
_DERIVED_DEFINES = frozenset({
    "BOARD_HAS_PSRAM", "HAS_DISPLAY", "DISPLAY_KIND", "DISPLAY_WIDTH",
    "DISPLAY_HEIGHT", "DISPLAY_ROTATION", "HAS_LVGL_UI",
})

_DISPLAY_KINDS = {"DISPLAY_KIND_OLED": "oled",
                  "DISPLAY_KIND_EINK": "eink",
                  "DISPLAY_KIND_TFT": "tft"}


def expected_capability(board: "Board") -> dict:
    """What the catalogue should say about this env, read from the build.

    The single definition of every capability claim. `check_boards.py` compares
    the registry against this and `tools/tests/test_board_catalogue.py` asserts
    the same thing, so the gate and the suite ask one question rather than
    growing two answers to it. `--print-capability` emits it, which is how a
    new board's entry is written: derived, never typed.
    """
    cap = {
        "mcu": board.mcu,
        "flash_mb": board.flash_mb,
        "psram": board.psram,
        "partitions": board.partitions,
    }
    if board.flag("HAS_DISPLAY"):
        cap["display"] = {
            "kind": _DISPLAY_KINDS.get(board.resolve_define("DISPLAY_KIND"), "unknown"),
            # The controller's own geometry, before any rotation. The prose
            # `display` field beside this one quotes the rotated figures for
            # two boards, which is why the rotation is carried here rather than
            # left for a reader to guess which convention a number follows.
            "width": board.number("DISPLAY_WIDTH"),
            "height": board.number("DISPLAY_HEIGHT"),
            "rotation": board.number("DISPLAY_ROTATION") or 0,
            # HAS_LVGL_UI, which is a graphical shell and not a touch layer.
            # Naming it `touch_ui` claimed a digitiser on the ThinkNode M9,
            # whose header says `HAS_TOUCH 0` and whose shell is driven by a
            # keyboard — a field contradicting two others about one board.
            "graphical_ui": board.flag("HAS_LVGL_UI"),
        }
    else:
        # Explicitly null, never absent and never a default. A board with no
        # panel inherits Config.h's 128x64 exactly as a 128-wide board does
        # (heltec_wb.h says `#define HAS_DISPLAY 0`), so recording the resolved
        # geometry here would put a measurement in the registry for hardware
        # that does not exist.
        cap["display"] = None
    cap["peripherals"] = {n: board.flag(f) for n, f in _PERIPHERAL_FLAG.items()}
    return cap


def _diff(path: str, got, want, problems: list, env: str) -> None:
    """Every difference, named by the path that differs."""
    if isinstance(want, dict) and isinstance(got, dict):
        for key in sorted(set(want) | set(got)):
            if key not in want:
                problems.append(f"{env}: capability.{path}.{key} is not a field this "
                                "catalogue derives — anything here that the build "
                                "cannot be asked about is a claim nothing checks")
            elif key not in got:
                problems.append(f"{env}: capability.{path}.{key} missing")
            else:
                _diff(f"{path}.{key}", got[key], want[key], problems, env)
        return
    if got != want:
        problems.append(f"{env}: capability.{path} says {got!r}, the build says {want!r}")


def validate_capability(registry: dict = None, ini: Ini = None,
                        warnings: list = None) -> list:
    """Problems with the catalogue's capability blocks. Empty means good.

    A whole-structure comparison rather than a ladder of per-field checks. The
    ladder let two kinds of falsehood through that the host test caught — a
    value for a key the build has no opinion on, and any invented key at all —
    which made the CI gate the weaker of the two callers that are supposed to
    be asking one question.
    """
    registry = registry if registry is not None else globals()["registry"]()
    ini = ini or Ini()
    problems = []
    warnings = warnings if warnings is not None else []
    for env in envs(registry):
        entry = registry[env]
        cap = entry.get("capability")
        if not isinstance(cap, dict):
            problems.append(f"{env}: no capability block — every env in the "
                            "catalogue needs one, or the registry cannot answer "
                            "what the board has")
            continue
        if not ini.has_env(env):
            continue                      # check_boards.py reports this already
        board = Board(env, ini)
        want = expected_capability(board)

        for key in sorted(set(want) | set(cap)):
            if key not in want:
                problems.append(f"{env}: capability.{key} is not a field this catalogue "
                                "derives — remove it, or teach board_facts.py the define "
                                "it follows so something can check it")
            elif key not in cap:
                problems.append(f"{env}: capability.{key} missing")
            else:
                _diff(key, cap[key], want[key], problems, env)

        if isinstance(want.get("display"), dict) and want["display"]["kind"] == "unknown":
            problems.append(
                f"{env}: DISPLAY_KIND is {board.resolve_define('DISPLAY_KIND')!r}, which "
                "board_facts._DISPLAY_KINDS does not name — add it, or the catalogue "
                "records a panel family it cannot describe")

        # A `-D` that the board header also defines unguarded is not an
        # override — the header wins, and the compiler says so with
        # -Wmacro-redefined, which this project treats as a defect anyway. It
        # is refused here rather than modelled, so the resolver's ordering
        # never has to be the thing standing between an author and a wrong
        # catalogue entry.
        clash = sorted(set(board.env_defines()) & board.header_defines())
        derived = _DERIVED_DEFINES | set(_PERIPHERAL_FLAG.values())
        for name in clash:
            if name in derived:
                problems.append(
                    f"{env}: passes -D{name} and boards/{board.header}.h also defines it "
                    "unguarded — the header wins, so the catalogue would record the "
                    "header's answer while the env asks for another. Make the header "
                    "#ifndef, or drop the flag")
            else:
                warnings.append(
                    f"{env}: passes -D{name} and boards/{board.header}.h also defines it "
                    "unguarded, so the flag is a no-op — the header wins and the compiler "
                    "warns. Harmless while the two agree; not an override")

        # The chip family is stated twice in this file: `chip` in the prose
        # block and `capability.mcu` derived from the manifest. Two statements
        # of one fact, so they are held to each other rather than left to
        # drift; retiring one of them is a larger change than this.
        chip = entry.get("chip")
        if chip and want["mcu"] and chip.replace("-", "").lower() != want["mcu"].lower():
            problems.append(f"{env}: chip says {chip!r} and capability.mcu says "
                            f"{want['mcu']!r} — one fact, two fields, disagreeing")
    return problems


def print_capability(env: str) -> str:
    """The block a new board's entry should carry, ready to paste.

    `docs/hardware.md` tells an author not to type the catalogue by hand; this
    is what it tells them to run instead. Without it the instruction was
    unfollowable — the checker names missing fields but prints no values, so
    the only way to learn nine peripheral booleans was to type nine wrong ones
    and read the complaints.
    """
    return json.dumps({"capability": expected_capability(Board(env))}, indent=2)


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("env", nargs="?", help="the platformio env to describe")
    ap.add_argument("--print-capability", action="store_true",
                    help="emit the capability block for ENV")
    args = ap.parse_args()
    if args.env:
        print(print_capability(args.env))
    else:
        for problem in validate_capability():
            print("boards.json:", problem)
