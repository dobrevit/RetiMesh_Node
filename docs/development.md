# Development

## Build
```sh
pipx install platformio
pio run -e t3s3                       # compile
pio run -e t3s3 -t upload             # flash firmware (asks a running node for its bootloader first)
pio run -e t3s3 -t uploadfs           # flash the web app (data/ → LittleFS)
pio device monitor                    # console, 115200
```
Seven board environments ship: `t3s3`, `t3s3-sx1280`, `t3s3-sx1280-pa`,
`esp32s3-qspi`, `tbeam`, `heltec-ws`, `heltec-v3`. CI and the release matrix
build all of them, so a change has to compile everywhere — including the three
with no SD slot, where `HAS_SD 0` has to actually work. `boards.json` is the
registry they come from. Add `-D` overrides under `build_flags`.

The platform is the [pioarduino](https://github.com/pioarduino/platform-espressif32)
fork of `espressif32`, pinned in `platformio.ini` to an exact release
(`55.03.311` is Arduino core 3.3.11 on ESP-IDF 5.5.5). PlatformIO's own
`espressif32` stopped at Arduino core 2.0.17 / ESP-IDF 4.4 and is not going to
move, so the fork is the only maintained route to core 3.x. The pin is exact
rather than a caret range because a platform bump is a toolchain bump — the
compiler, the prebuilt IDF libraries and the core's API all move together — and
CI has to build what was qualified; `tools/bump_deps.py` leaves it alone and it
is moved by hand. The first build fetches the platform, the core, its prebuilt
libraries and the GCC 14 toolchain from GitHub, a few hundred megabytes.

The fork keeps one copy of the core per package directory: on every run it
deletes any `framework-arduinoespressif32@<version>` package it finds, and it
installs its own `tool-esptoolpy`, `tool-scons` and toolchains under the same
names the official platform uses. It therefore cannot share `~/.platformio`
with a checkout on the official platform (the RNode firmware mirror, or an
older branch of this repo) without the two reinstalling each other's packages
on every switch — and a build running in one checkout while the other
installs is broken mid-way. On a bench that holds both toolchains, give the
core-3 checkout its own package and platform directories on every `pio`
command (`run`, `test`, `pkg`, `envdump`):

```sh
PLATFORMIO_PACKAGES_DIR=$HOME/.platformio/packages-core3 \
PLATFORMIO_PLATFORMS_DIR=$HOME/.platformio/platforms-core3 \
pio run -e t3s3
```

The first run downloads everything again into those directories; that is the
point. A bench with only this repo on it needs neither variable.

Shared build settings live in named sections rather than per-board copies:
`[esp32s3]` for what any S3 wants, `[esp32s3_psram_usb]` for the four boards
that also have in-package PSRAM and the S3's own USB on the connector. The
Heltec V3 has neither, which is why it takes the plain one.

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

### Uploading
`-t upload` runs `tools/upload_hook.py`: it asks the running node for its
bootloader over the maintenance console (or `POST /api/system/bootloader` at
`$RETIMESH_NODE_URL`), waits for the port to come back, lets esptool flash,
and waits for the application to answer `VERSION` again. Every step is bounded
and every failure is a message; esptool's own DTR/RTS reset remains the
fallback and `RETIMESH_NO_AUTO_BOOTLOADER=1` skips the hand-off. With several
boards attached pass `--upload-port`. See [local-link.md](local-link.md#flashing).

## Hardware-in-the-loop checks
With the node on USB and a local `rnsd` + RNode on the same channel:
```sh
python tools/hil.py --port /dev/serial/by-id/<node> --rns-bin ~/venv/bin --reset
```
Checks boot (identity, radio, transport, no error lines), that rnsd holds a
path to the node, that plain packets sent through rnsd arrive on LoRa, and
that the RNode hears the node. Exit code = number of failures.

`tools/hil_bootloader.py --port … [--ip …] [--firmware …]` exercises the
maintenance console, the link listing, the bootloader transition, esptool
reaching the ROM, a flash and the application's return. Both run from
`.github/workflows/hil.yml` on a self-hosted runner labelled `retimesh-hil`
(dispatch only; ordinary CI never touches hardware).
`sudo tools/hil_ppp.py --port … [--firmware …] [--chip esp32]` does the same
round over PPP on a bridged board — `PPP ON`, pppd up, the API over ppp0,
the bootloader request over ppp0, pppd exiting as the node goes down,
esptool, the application back, ppp0 up again — and needs root for pppd, so
it is run by hand rather than from the workflow.

## Soak testing
A soak is only worth running if someone reads the result, and a week of JSON is
not something anyone reads. `tools/soak.py` samples every node by its mDNS name
and summarises what a soak is actually asked:

```sh
python tools/soak.py --out soak.csv retimesh-8249cc retimesh-cd5a28   # collect
python tools/soak.py --summarise soak.csv                             # read it
```

It reports restarts and why, the heap trend with its low-water mark and largest
block, the lowest stack headroom by task name, table growth, and the five loss
counters as deltas. Every one of those has caught something real on this bench.
A node that misses a poll is recorded as absent and the run continues, because
one unreachable node is a finding rather than a reason to stop collecting from
the others.

Two signatures worth knowing. A **falling largest block against a healthy free
heap** is fragmentation, and it ends with a node that still routes but can no
longer build a status response — it goes quiet on HTTP and mDNS with its boot
count frozen. And **a reset reason of power-on with the previous run length
missing** means the RTC domain was lost, which is what an EN-pin reset from a
USB bridge looks like: opening the console on a CH34x board resets it, so a
"restart" in the data may be the person watching it.

## What each subsystem costs
The boot log carries a bill. After each subsystem starts, `Diag::cost()` logs
what it took of the RAM that decides — byte-addressable internal RAM, the kind
a task stack has to come from — where DRAM stands afterwards, and the running
total:

```
cost: littlefs         +2032 B  (213572 free, 110580 largest, +2032 B since boot)
cost: settings          +336 B  (213236 free, 110580 largest, +2368 B since boot)
cost: power               +0 B  (213236 free, 110580 largest, +2368 B since boot)
cost: packet rings    +26916 B  (186320 free, 110580 largest, +29284 B since boot)
cost: identity            +0 B  (186320 free, 110580 largest, +29284 B since boot)
cost: display          +2484 B  (183836 free, 110580 largest, +31768 B since boot)
cost: wifi radio      +52500 B  (131336 free, 110580 largest, +84268 B since boot)
cost: http + dns + mdns  +28424 B  (102912 free, 102388 largest, +112692 B since boot)
cost: local links         +0 B  (102912 free, 102388 largest, +112692 B since boot)
cost: lora radio       +1216 B  (101696 free,  98292 largest, +113908 B since boot)
cost: reticulum       +52668 B  ( 49028 free,  45044 largest, +166576 B since boot)
cost: rns tcp server    +272 B  ( 48756 free,  45044 largest, +166848 B since boot)
cost: autointerface    +9188 B  ( 39568 free,  36852 largest, +176036 B since boot)
cost: tasks           +35020 B  (  4548 free,   4084 largest, +211056 B since boot)
```

That is a real Heltec Wireless Stick, and the last line is what it has left to
run on: **4548 bytes**. It is not enough. That board throws `std::bad_alloc`
out of the Reticulum loop within seconds of finishing boot and panics, and its
`boot_count` runs into the hundreds. Nothing else the node reports says so —
`heap_free` reads a comfortable 46 KB, because most of what is left is
32-bit-only IRAM that no allocation can use.

Read the rest as the answer to "which switch is worth making cost nothing when
it is off", per board. Wi-Fi is billed in two parts on purpose: the **radio**
is what the switch is meant to buy, and **http + dns + mdns** is what a node
pays whether Wi-Fi is on or off — so on this board, switching Wi-Fi off saves
52 KB and still leaves 28 KB on the table for a web server nobody can reach.
That, and not USB-NCM, is where lazy allocation is worth the work.

Two more things this reading settled. `packet rings` costs 26 KB of scarce
internal RAM on every board **without** PSRAM, because `psramRing()` falls back
to the internal heap and the fallback is silent. And `local links` reads 0 here
but 10556 B on a Heltec V3 with `links.ppp` on — the PPP interface and its
reader task, which is what that switch now gives back.

The numbers differ enough between boards that one board's answer is not
another's, which is why this is measured per board rather than reasoned about
once. It exists because the alternative is estimating, and estimating is how a
Wireless Stick came to spend about 16 KB on a PPP link whose switch was off —
almost exactly the `rns` stack it then could not place, so it ran with no
Reticulum at all while reporting `transport: online`.

A negative figure is a credit: a subsystem that probed for hardware, found none
and handed back what it took to look.

## Unit tests
Pure headers are tested on the host, no hardware needed:
```sh
pio test -e native
```
`test/stubs/` provides the few Arduino/IDF headers those files expect. CI runs
them on every push.

| Suite | Covers |
|---|---|
| `test_hdlc` | RNS TCP framing |
| `test_airtime` | duty cycle, dwell budget, CSMA accounting |
| `test_radio_plan` | per-chip radio limits, regional regimes, node naming |
| `test_store_home` | where the Reticulum store belongs, card ownership, what a move does |
| `test_local_link` | the local-link phase machine and the host-facing trust rule |
| `test_bootloader` | which bootloader methods a board offers, the restart sequence and its re-arm rule |
| `test_maintenance` | the console protocol: parsing, malformed and overlong lines, noise, replies |

The host tooling has its own suite, run by CI too:
```sh
cd tools/retimesh-flash && python -m unittest discover -s tests -t .
```
It drives `retimesh_flash.device` — port discovery, the console protocol, the
bootloader hand-off in every outcome, HTTP probing — against fake ports and a
fake node, so the flashing workflow is tested without a board.

The pattern worth keeping: a rule that decides something consequential is
written as a pure function in a header with no Arduino dependency, so it can be
exercised without a board. `StoreHome::decide()` and `Mdns::label()` are there
for that reason — both used to be inline in code whose only test was flashing a
node and reading the log.

## Layout
```
src/            firmware (single PlatformIO project, Arduino framework)
data/           web app → LittleFS image
web/            GitHub Pages web flasher (ESP Web Tools)
test/           host-side unit tests (see above)
partitions/     huge_app_8mb.csv, for the 8 MB boards with no SD slot
tools/          make_manifest.py (release bundles), build_site.py (Pages),
                bump_deps.py (PlatformIO dependency PRs), retimesh-flash/ (CLI
                and the shared device/bootloader module), hil.py and
                hil_bootloader.py (hardware-in-the-loop), soak.py (fleet
                sampler and summariser), asset_stamp.py (build-time web asset
                hash), board_caps.py (boards.json -> BOARD_* flags),
                check_boards.py (boards.json consistency, CI),
                board_docs.py (boards.json -> the board matrix in docs/hardware.md, CI checks it),
                upload_hook.py (bootloader hand-off around `-t upload`)
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
- Serial ports swap on replug: use `/dev/serial/by-id/…` (CP2102 boards all
  report serial `0001`, so `/dev/serial/by-path/…` for those).
- The serial port answers commands as well as printing the log: `VERSION`,
  `STATUS`, `NETWORK_STATUS`, `BOOTLOADER CONFIRM` — replies start with `RM `.
  See [local-link.md](local-link.md#the-maintenance-console).

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

Touching `src/LoRaRadio.*`, `src/net/HDLC.h` or `src/RetiTransportServer.*` also
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
