# Development

## Build
```sh
pipx install platformio
pio run -e t3s3                       # compile
pio run -e t3s3 -t upload             # flash firmware
pio run -e t3s3 -t uploadfs           # flash the web app (data/ → LittleFS)
pio device monitor                    # console, 115200
```
Envs: `t3s3` (LilyGO T3-S3, `qio_qspi`, 4 MB, `huge_app.csv`),
`esp32s3-qspi` (generic 8 MB DevKit). Add `-D` overrides under `build_flags`.

`PLATFORMIO_BUILD_FLAGS='-DFW_VERSION=\"v1.2.3\"'` bakes a version (CI does this
from the tag).

### Library dependencies and the forks

microReticulum and microStore are pulled from our forks
(`dobrevit/microReticulum#retimesh/combined`,
`dobrevit/microStore#fix/close-active-segment-before-compaction`) until the
upstream PRs land: attermann/microReticulum#82 (configurable housekeeping
interval), #85 (packet-carrying `AnnounceHandler` callback) and
attermann/microStore#6 (compaction closes the active segment). The firmware
relies on those APIs (`Reticulum::jobs_interval()`, the 4-argument
`received_announce`). Once merged, point `lib_deps` back at upstream.

To hack on the libraries themselves, `[env:t3s3-local]` builds against sibling
checkouts (`../microReticulum`, `../microStore`) instead of git:

```sh
pio run -e t3s3-local -t upload --upload-port /dev/serial/by-id/<node>
```

## Hardware-in-the-loop checks
With the node on USB and a local `rnsd` + RNode on the same channel:
```sh
python tools/hil.py --port /dev/serial/by-id/<node> --rns-bin ~/venv/bin --reset
```
Checks boot (identity, radio, transport, no error lines), that rnsd holds a
path to the node, that plain packets sent through rnsd arrive on LoRa, and
that the RNode hears the node. Exit code = number of failures.

## Unit tests
Pure headers are tested on the host, no hardware needed:
```sh
pio test -e native
```
`test/stubs/` provides the few Arduino/IDF headers those files expect.
Current suites: `test_hdlc` (RNS TCP framing). CI runs them on every push.

## Layout
```
src/            firmware (single PlatformIO project, Arduino framework)
data/           web app → LittleFS image
web/            GitHub Pages web flasher (ESP Web Tools)
tools/          make_manifest.py (release bundles), build_site.py (Pages),
                bump_deps.py (PlatformIO dependency PRs), retimesh-flash/ (CLI)
boards.json     board registry used by CI, packaging, flasher and CLI
docs/           this documentation
.github/        CI, Release Drafter, tag-driven releases, Pages, Dependabot
```

## Debugging
- Console verbosity: `CORE_DEBUG_LEVEL` (Arduino, 3 = info) and the
  microReticulum runtime level in `RnsTransport.cpp` (`RNS::loglevel(...)`;
  DEBUG is compiled in). At DEBUG, Transport prints every announce decision
  ("Rebroadcasting announce for …", "Blocking …").
- Backtraces: `xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/t3s3/firmware.elf <addrs>`.
- On the RNS side: `rnstatus` (interface counters, violations), `rnpath -t`
  (path table — the node and its clients should appear with hop counts),
  `rnid -i <identity> -a <aspect>` to emit test announces.
- Serial ports swap on replug: use `/dev/serial/by-id/…`.

## Threading rules
- Only the **rns task** calls into microReticulum.
- Only the **radio task** touches the transceiver.
- AsyncTCP callbacks copy bytes into rings and post events; nothing heavy.
- Web handlers read snapshots (`RnsTransport::paths/interfaces`), never
  Transport tables directly.

## Branches and pull requests

Work lands on `main` through pull requests, never by pushing directly. The
branch prefix and the PR title are not decoration: Release Drafter reads both.

| Prefix | Label it gets | Release section | Version bump |
|---|---|---|---|
| `feat/…` | `feature` | 🚀 Features | minor |
| `fix/…`, `bugfix/…`, `hotfix/…` | `fix` | 🐛 Fixes | patch |
| `docs/…` | `docs` | 📖 Documentation | patch |
| `ci/…` | `ci` | 🧰 CI & tooling | patch |

Touching `src/LoRaRadio.*`, `src/HDLC.h` or `src/RetiTransportServer.*` also
adds `radio`, which files the change under 📻 Radio / protocol and makes the
release a minor one. A `!` after the type in the title (`feat!: …`) marks a
breaking change and bumps the major version. Add `skip-changelog` to a PR that
should not appear in the notes at all.

**The PR title becomes the release-note line**, verbatim, as
`- <title> (#<number>) @<author>`. Write it for someone deciding whether to
upgrade, not for your future self reading `git log`: say what changed for the
operator of a node.

```
feat: keep the Reticulum store on the SD card so it survives reboots
fix: stop the SD store falling back to internal flash at boot
docs: explain the duty-cycle limiter and channel access
```

A conventional-commit prefix in the title (`feat:`, `fix:`, `docs:`, `ci:`)
labels the PR on its own, so it works even when the branch is named something
else. Commit messages inside the branch are for reviewers and can be as
detailed as they need to be.

## Releases
Release Drafter keeps a draft up to date as pull requests merge, so the notes
are written by the time they are needed — one line per PR, in the section its
label selected. Push a tag `vX.Y.Z` → CI builds every board, packages bundles,
attaches them to that draft with a commit list → smoke-test the merged image →
**Publish** → the Pages workflow redeploys the web flasher. Details in the
README's *Development workflow* table.

## Contributing
Branch and PR conventions are above. Keep the
threading rules, the RNode wire format and RNS framing byte-exact, and update
docs/settings/API alongside code. New boards: `platformio.ini` env +
`boards.json` + workflow matrices + a docs/hardware.md row. License:
GPL-3.0-or-later; add the SPDX header to new files.
