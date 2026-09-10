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

"""The catalogue agrees with `platformio.ini`, the manifests and the headers.

`tools/check_boards.py` gates the same rule in CI. This suite exists beside it
for two reasons: it runs under `unittest discover`, which is where the rest of
the tooling's tests live, and it can assert that the checker *rejects* as well
as accepts — a validator nobody has watched fail is a validator that might not.
"""

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import board_facts as bf  # noqa: E402


class CatalogueAgreesWithTheBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reg = bf.registry()
        cls.ini = bf.Ini()

    def test_the_shipped_catalogue_has_no_problems(self):
        problems = bf.validate_capability(self.reg, self.ini)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_every_env_has_a_capability_block(self):
        for env in bf.envs(self.reg):
            self.assertIn("capability", self.reg[env], f"{env} has no capability block")

    def test_every_capability_block_is_what_the_build_resolves(self):
        # The clause this suite is named for: catalogue and platformio.ini
        # agree, asserted per board rather than in aggregate so a failure names
        # the one that drifted.
        for env in bf.envs(self.reg):
            with self.subTest(env=env):
                want = bf.expected_capability(bf.Board(env, self.ini))
                self.assertEqual(self.reg[env]["capability"], want)


class TheCheckerRejects(unittest.TestCase):
    """A gate that has never been seen to fail is not known to be a gate."""

    def setUp(self):
        self.reg = copy.deepcopy(bf.registry())
        self.ini = bf.Ini()
        self.env = "t3s3"

    def _problems(self):
        return bf.validate_capability(self.reg, self.ini)

    def test_a_wrong_psram_claim_is_caught(self):
        cap = self.reg[self.env]["capability"]
        cap["psram"] = not cap["psram"]
        self.assertTrue(any("psram" in p for p in self._problems()))

    def test_a_wrong_flash_size_is_caught(self):
        self.reg[self.env]["capability"]["flash_mb"] = 999
        self.assertTrue(any("flash_mb" in p for p in self._problems()))

    def test_a_missing_field_is_caught(self):
        del self.reg[self.env]["capability"]["mcu"]
        self.assertTrue(any("mcu missing" in p for p in self._problems()))

    def test_a_missing_block_is_caught(self):
        del self.reg[self.env]["capability"]
        self.assertTrue(any("no capability block" in p for p in self._problems()))

    def test_a_wrong_partition_table_is_caught(self):
        self.reg[self.env]["capability"]["partitions"] = "partitions/not_this.csv"
        self.assertTrue(any("partitions" in p for p in self._problems()))

    def test_geometry_invented_for_a_board_with_no_panel_is_caught(self):
        # heltec-wb has `#define HAS_DISPLAY 0`. Filling in the geometry the
        # build would resolve anyway — Config.h's 128x64 default — is exactly
        # the mistake the null is there to prevent.
        self.reg["heltec-wb"]["capability"]["display"] = {
            "kind": "oled", "width": 128, "height": 64, "touch_ui": False}
        self.assertTrue(any("heltec-wb" in p and "display" in p
                            for p in self._problems()))

    def test_a_peripheral_claim_the_headers_contradict_is_caught(self):
        periph = self.reg[self.env]["capability"]["peripherals"]
        periph["gnss"] = not periph["gnss"]
        self.assertTrue(any("peripherals.gnss" in p for p in self._problems()))

    def test_a_peripheral_nothing_can_check_is_caught(self):
        self.reg[self.env]["capability"]["peripherals"]["jetpack"] = True
        self.assertTrue(any("jetpack" in p for p in self._problems()))

    def test_a_wrong_partition_table_on_a_manifest_default_env_is_caught(self):
        # esp32s3-qspi names no board_build.partitions and inherits
        # default_8MB.csv from its manifest. The first version of the checker
        # guarded the whole comparison on the *expected* value being truthy,
        # so any value at all was accepted here — the one hole a review found
        # by reproducing it rather than by reading.
        self.reg["esp32s3-qspi"]["capability"]["partitions"] = "partitions/nope.csv"
        self.assertTrue(any("esp32s3-qspi" in p and "partitions" in p
                            for p in self._problems()))

    def test_an_invented_top_level_field_is_caught(self):
        # A key the build has no opinion on is a claim nothing can check, which
        # is how a catalogue starts lying.
        self.reg[self.env]["capability"]["radio"] = "a jetpack"
        self.assertTrue(any("radio" in p for p in self._problems()))

    def test_an_invented_display_field_is_caught(self):
        self.reg[self.env]["capability"]["display"]["backlight"] = True
        self.assertTrue(any("display.backlight" in p for p in self._problems()))

    def test_a_missing_display_key_is_caught(self):
        del self.reg[self.env]["capability"]["display"]
        self.assertTrue(any("display missing" in p for p in self._problems()))

    def test_a_peripherals_block_that_is_not_a_mapping_is_caught(self):
        self.reg[self.env]["capability"]["peripherals"] = "all of them"
        self.assertTrue(any("peripherals" in p for p in self._problems()))

    def test_a_wrong_rotation_is_caught(self):
        # The field exists because the prose `display` for two boards quotes
        # the rotated geometry while the catalogue quotes the controller's.
        self.reg["thinknode-m9"]["capability"]["display"]["rotation"] = 0
        self.assertTrue(any("rotation" in p for p in self._problems()))

    def test_a_chip_that_contradicts_capability_mcu_is_caught(self):
        # Two statements of one fact in the same file, held to each other.
        self.reg[self.env]["chip"] = "esp32"
        self.assertTrue(any("one fact, two fields" in p for p in self._problems()))

    def test_an_untouched_registry_still_passes(self):
        # The counterweight: every case above must fail for the reason it
        # names, not because the checker refuses everything.
        self.assertEqual(self._problems(), [])


if __name__ == "__main__":
    unittest.main()
