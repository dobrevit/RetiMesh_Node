# Vendored PlatformIO board manifests

The four board definitions `platformio.ini` names, copied out of
`platform-espressif32` so that they are readable without it.

They are here because two facts about every board live only in these files and
nowhere in this repository: the chip family, and the flash size. The T-Beam's
`BOARD_HAS_PSRAM` is a third — it is in `ttgo-t-beam.json`'s `build.extra_flags`
and in no `platformio.ini` section, which is why a board with 4 MB of PSRAM
looks like it has none to anything that reads only this repo.

`tools/board_facts.py` resolves the capability catalogue from them and
`tools/check_boards.py` gates the registry against the answer. Both run in CI's
`tools` job, which installs neither PlatformIO nor the platform — so without
these copies the gate fails every board for want of a manifest, which is
exactly backwards from what it is for.

`tools/make_manifest.py` already looked here first (`board_json()`), for the
same reason and before this directory existed.

**Keeping them honest.** They are a copy, so they can go stale when the pinned
platform moves. `tools/tests/test_board_facts.py` compares each one against the
installed platform's copy when there is one and skips when there is not, so a
developer with the toolchain sees the drift and CI does not fail for lacking
it. The pinned platform version is in `platformio.ini`; update these together
with it.
