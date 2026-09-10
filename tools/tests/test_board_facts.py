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

"""The resolver behind the capability catalogue.

Split deliberately between synthetic cases and real-tree cases. The synthetic
ones pin the *rules* — precedence, interpolation, the shapes a manifest can
take — on fixtures written here, so they keep testing the rule when the fleet
changes. The real-tree ones pin the handful of answers that are known
independently of this code, from `platformio.ini` and the project brief, so a
resolver that quietly stopped resolving would be caught rather than agreeing
with itself.
"""

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import board_facts as bf  # noqa: E402


def _ini(text: str, tmp: Path) -> bf.Ini:
    p = tmp / "platformio.ini"
    p.write_text(textwrap.dedent(text))
    return bf.Ini(p)


class DefineExtraction(unittest.TestCase):
    def test_a_bare_define_is_one(self):
        self.assertEqual(bf._defines("-DFOO"), {"FOO": "1"})

    def test_a_valued_define_keeps_its_value(self):
        self.assertEqual(bf._defines("-DFOO=7"), {"FOO": "7"})

    def test_a_list_of_flags_reads_the_same_as_a_string(self):
        # PlatformIO's board manifests write build.extra_flags both ways —
        # ttgo-t-beam.json as a string, esp32-s3-devkitc-1.json as a list —
        # and assuming one shape raised a TypeError on the other.
        self.assertEqual(bf._defines(["-DA", "-DB=2"]), bf._defines("-DA -DB=2"))

    def test_flags_that_are_not_defines_are_ignored(self):
        self.assertEqual(bf._defines("-Wall -Os -DFOO -I include"), {"FOO": "1"})


class Interpolation(unittest.TestCase):
    """PlatformIO's `${section.option}`, which is not configparser's."""

    def setUp(self):
        # A real temporary directory, not one under the repository: a test
        # that leaves a platformio.ini behind in fixtures/ is a test that
        # eventually gets committed by somebody staging a whole directory.
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)

    def test_a_flag_reached_through_a_base_section_is_found(self):
        # The trap this whole module exists for: read the env's raw
        # build_flags and four boards report no PSRAM while carrying megabytes
        # of it.
        ini = _ini("""
            [psram_base]
            build_flags = -DBOARD_HAS_PSRAM

            [env:board_a]
            build_flags = ${psram_base.build_flags} -DOTHER
        """, self.tmp)
        self.assertIn("BOARD_HAS_PSRAM", bf._defines(ini.build_flags("board_a")))

    def test_an_env_without_the_base_section_does_not_get_the_flag(self):
        ini = _ini("""
            [psram_base]
            build_flags = -DBOARD_HAS_PSRAM

            [env:board_b]
            build_flags = -DOTHER
        """, self.tmp)
        self.assertNotIn("BOARD_HAS_PSRAM", bf._defines(ini.build_flags("board_b")))

    def test_extends_contributes_its_flags(self):
        ini = _ini("""
            [env:base_env]
            build_flags = -DFROM_BASE

            [env:child]
            extends = env:base_env
            build_flags = -DFROM_CHILD
        """, self.tmp)
        got = bf._defines(ini.build_flags("child"))
        self.assertIn("FROM_BASE", got)
        self.assertIn("FROM_CHILD", got)

    def test_an_interpolation_cycle_ends_instead_of_hanging(self):
        # A cycle is a mistake rather than a design, and a bounded walk turns
        # it into a short answer rather than a CI job that never returns.
        ini = _ini("""
            [a]
            build_flags = ${b.build_flags}

            [b]
            build_flags = ${a.build_flags}

            [env:looper]
            build_flags = ${a.build_flags} -DREAL
        """, self.tmp)
        self.assertIn("REAL", bf._defines(ini.build_flags("looper")))

    def test_option_follows_extends_for_a_plain_value(self):
        ini = _ini("""
            [env:base_env]
            board = some-board

            [env:child]
            extends = env:base_env
        """, self.tmp)
        self.assertEqual(ini.option("child", "board"), "some-board")


class Precedence(unittest.TestCase):
    """Env `-D`, then the board header, then Config.h's default."""

    def setUp(self):
        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        self.tmp = Path(self._tmpdir.name)
        self.config_h = textwrap.dedent("""
            #ifndef HAS_THING
              #define HAS_THING 0
            #endif
            #ifndef WIDGET_SIZE
              #define WIDGET_SIZE 12
            #endif
            #if defined(BOARD_FAKE)
              #include "boards/fake.h"
            #endif
        """)

    def _board(self, flags: str):
        ini = _ini(f"""
            [env:fake]
            build_flags = {flags}
        """, self.tmp)
        return bf.Board("fake", ini, self.config_h)

    def test_config_h_default_is_the_floor(self):
        b = self._board("")
        self.assertEqual(b.resolve_define("WIDGET_SIZE"), "12")
        self.assertFalse(b.flag("HAS_THING"))

    def test_an_env_define_beats_the_config_default(self):
        # HAS_LVGL_UI reaches its three boards exactly this way and no other,
        # so a resolver that skipped this source would call every board
        # non-touch.
        b = self._board("-DHAS_THING=1")
        self.assertTrue(b.flag("HAS_THING"))

    def test_an_unknown_define_is_none_rather_than_a_guess(self):
        self.assertIsNone(self._board("").resolve_define("NO_SUCH_THING"))

    def test_zero_is_false_and_not_merely_present(self):
        # `#define HAS_DISPLAY 0` is how a board says it has no panel; reading
        # the define's presence rather than its value would make every board
        # look like it had one.
        b = self._board("-DHAS_THING=0")
        self.assertFalse(b.flag("HAS_THING"))

    def test_number_parses_and_tolerates_a_non_number(self):
        b = self._board("-DWIDGET_SIZE=0x40 -DHAS_THING=nonsense")
        self.assertEqual(b.number("WIDGET_SIZE"), 64)
        self.assertIsNone(b.number("HAS_THING"))


class AgainstTheRealTree(unittest.TestCase):
    """Answers known from somewhere other than this code."""

    @classmethod
    def setUpClass(cls):
        cls.reg = bf.registry()
        cls.ini = bf.Ini()
        cls.boards = {e: bf.Board(e, cls.ini) for e in bf.envs(cls.reg)}

    def test_every_registry_env_exists_in_platformio_ini(self):
        for env in self.boards:
            self.assertTrue(self.ini.has_env(env), f"{env} has no [env:{env}]")

    def test_the_no_psram_boards_are_the_ones_the_ini_says(self):
        # Ground truth is `${esp32s3_psram.build_flags}` in platformio.ini,
        # which the project brief names as the way to tell, and these three
        # are the boards the brief and docs/hardware.md call no-PSRAM.
        without = {e for e, b in self.boards.items() if not b.psram}
        self.assertEqual(without, {"heltec-v3", "heltec-wp", "heltec-ws"})

    def test_the_tbeam_gets_psram_from_its_manifest_not_the_ini(self):
        # Nothing in platformio.ini gives the T-Beam this flag; its
        # PlatformIO board file does, in build.extra_flags. Reading only the
        # repository would report a PSRAM board as having none.
        self.assertTrue(self.boards["tbeam"].psram)
        self.assertNotIn("BOARD_HAS_PSRAM",
                         bf._defines(self.ini.build_flags("tbeam")))

    def test_lvgl_is_on_exactly_the_touch_boards(self):
        lvgl = {e for e, b in self.boards.items() if b.flag("HAS_LVGL_UI")}
        self.assertEqual(lvgl, {"heltec-v4", "t-deck", "thinknode-m9"})

    def test_a_board_with_no_panel_reports_none(self):
        # heltec_wb.h says `#define HAS_DISPLAY 0`. Geometry still resolves,
        # to Config.h's default, which is why callers must gate on the flag.
        self.assertFalse(self.boards["heltec-wb"].flag("HAS_DISPLAY"))

    def test_every_board_resolves_an_mcu_and_a_flash_size(self):
        for env, b in self.boards.items():
            self.assertIn(b.mcu, ("esp32", "esp32s3"), f"{env}: mcu {b.mcu!r}")
            self.assertIn(b.flash_mb, (4, 8, 16), f"{env}: flash {b.flash_mb!r}")

    def test_each_env_resolves_to_a_header_that_exists(self):
        for env, b in self.boards.items():
            p = bf.ROOT / "src" / "boards" / f"{b.header}.h"
            self.assertTrue(p.exists(), f"{env} -> {b.header}.h missing")


class PeripheralGroundTruth(unittest.TestCase):
    """Facts known from somewhere other than the resolver or the catalogue.

    This is the seam that breaks the circle. `test_board_catalogue.py` compares
    `boards.json` against the function that generated it, so it cannot catch a
    resolver that is wrong in the same direction as the data it produced. These
    assertions come from the registry's own hand-written `extras` prose and the
    bench notes behind it — written by people looking at the boards, years
    before this module existed — and go straight at `Board.flag`, touching
    neither `expected_capability` nor the catalogue.
    """

    @classmethod
    def setUpClass(cls):
        cls.ini = bf.Ini()

    def _assert(self, env, **expected):
        b = bf.Board(env, self.ini)
        for name, want in expected.items():
            with self.subTest(env=env, flag=name):
                self.assertEqual(b.flag(name), want,
                                 f"{env}: {name} resolved {b.flag(name)}, "
                                 f"the board's own notes say {want}")

    def test_the_wireless_paper_has_neither_card_nor_receiver(self):
        # boards.json extras: "no SD, no GNSS".
        self._assert("heltec-wp", HAS_SD=False, HAS_GPS=False)

    def test_the_tbeam_has_a_receiver_and_a_pmu_and_no_card(self):
        # boards.json extras: u-blox GPS, AXP PMU, no SD slot.
        self._assert("tbeam", HAS_GPS=True, HAS_PMU=True, HAS_SD=False)

    def test_the_supreme_carries_the_five_parts_its_notes_name(self):
        # extras: GNSS, microSD, AXP2101 PMU, QMI8658 IMU, QMC6310N
        # magnetometer, BME280 — the bring-up recorded each one on hardware.
        self._assert("tbeam-supreme", HAS_GPS=True, HAS_SD=True, HAS_PMU=True,
                     HAS_IMU=True, HAS_COMPASS=True, HAS_ENV=True)

    def test_the_m9_has_a_keyboard_and_no_touch_layer(self):
        # extras: ATGM336H GNSS, microSD, sounder, IMU, magnetometer, and a
        # keyboard rather than a digitiser. The distinction is the reason
        # display.graphical_ui is not called touch_ui.
        self._assert("thinknode-m9", HAS_GPS=True, HAS_SD=True, HAS_IMU=True,
                     HAS_COMPASS=True, HAS_BUZZER=True, HAS_PMU=False,
                     HAS_TOUCH=False)
        self.assertTrue(bf.Board("thinknode-m9", self.ini).flag("HAS_LVGL_UI"))

    def test_the_v4_carries_the_expansion_sensors(self):
        # Driven and bench-confirmed on 2026-09-10: a BME280 at 0x76 and a
        # GXHT3V at 0x70, beside the DA217 and the sounder.
        self._assert("heltec-v4", HAS_ENV=True, HAS_IMU=True, HAS_BUZZER=True,
                     HAS_GPS=True, HAS_TOUCH=True, HAS_SD=False)


class HeaderMapping(unittest.TestCase):
    """Which header an env builds — pinned by name, not by "a file exists"."""

    @classmethod
    def setUpClass(cls):
        cls.ini = bf.Ini()

    def test_each_env_maps_to_the_header_its_flag_dispatches_to(self):
        # Asserting the mapping rather than the existence of whatever it
        # returned: a resolver that always answered "t3s3" passed the
        # file-exists check, because t3s3.h exists.
        for env, header in (("thinknode-m9", "thinknode_m9"), ("t-deck", "tdeck"),
                            ("heltec-v4", "heltec_v4"), ("heltec-wb", "heltec_wb"),
                            ("tbeam-supreme", "tbeam_supreme"),
                            ("t3s3-sx1280-pa", "t3s3_sx1280_pa")):
            with self.subTest(env=env):
                self.assertEqual(bf.Board(env, self.ini).header, header)

    def test_an_env_with_no_board_flag_builds_the_chains_else_arm(self):
        # t3s3 and esp32s3-qspi pass no BOARD_* flag by design.
        for env in ("t3s3", "esp32s3-qspi"):
            with self.subTest(env=env):
                self.assertEqual(bf.Board(env, self.ini).header, "t3s3")

    def test_the_prefix_alike_pair_does_not_collide(self):
        # Config.h tests BOARD_T3S3_SX1280_PA before BOARD_T3S3_SX1280 because
        # one name contains the other; resolving in build_flags order rather
        # than chain order could pick either.
        self.assertEqual(bf.Board("t3s3-sx1280", self.ini).header, "t3s3_sx1280")
        self.assertEqual(bf.Board("t3s3-sx1280-pa", self.ini).header, "t3s3_sx1280_pa")


class VendoredManifests(unittest.TestCase):
    """The copies under boards/ against the platform's own, where installed."""

    def test_each_vendored_manifest_matches_the_installed_platform(self):
        import json as _json
        checked = 0
        for path in sorted((bf.ROOT / "boards").glob("*.json")):
            board = path.stem
            installed = [p for p in bf.manifest_paths(board)[1:] if p.exists()]
            if not installed:
                continue
            checked += 1
            with self.subTest(board=board):
                self.assertEqual(_json.loads(path.read_text()),
                                 _json.loads(installed[0].read_text()),
                                 f"boards/{board}.json has drifted from the pinned "
                                 "platform's copy — see boards/README.md")
        if not checked:
            self.skipTest("no PlatformIO platform installed to compare against")


if __name__ == "__main__":
    unittest.main()
