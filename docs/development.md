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

### Building against the upstream fix branches

`[env:t3s3-upstream]` builds the firmware against local checkouts of
microReticulum and microStore that carry fixes not yet merged upstream
(configurable `Transport::jobs()` interval, packet-carrying announce callback,
compaction closing the active segment). It expects the two repositories as
siblings of this one (`../microReticulum`, `../microStore`) and sets
`RETIMESH_UPSTREAM_FIXES=1`, which switches `RnsTransport.cpp` to the new
library APIs. The stock `t3s3` env keeps working against the registry versions.

```sh
pio run -e t3s3-upstream -t upload --upload-port /dev/serial/by-id/<node>
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

## Releases
Push a tag `vX.Y.Z` → CI builds every board, packages bundles, attaches them
to the drafted release with a commit list → smoke-test the merged image →
**Publish** → the Pages workflow redeploys the web flasher. Details in the
README's *Development workflow* table.

## Contributing
Branch prefixes `feat/`, `fix/`, `docs/`, `ci/` (autolabelled). Keep the
threading rules, the RNode wire format and RNS framing byte-exact, and update
docs/settings/API alongside code. New boards: `platformio.ini` env +
`boards.json` + workflow matrices + a docs/hardware.md row. License:
GPL-3.0-or-later; add the SPDX header to new files.
