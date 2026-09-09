# Development

## Build
```sh
pipx install platformio
pio run -e t3s3                       # compile
pio run -e t3s3 -t upload             # flash firmware (asks a running node for its bootloader first)
pio run -e t3s3 -t uploadfs           # flash the web app (data/ → LittleFS)
pio device monitor                    # console, 115200
```
Thirteen board environments ship: `t3s3`, `t3s3-sx1280`, `t3s3-sx1280-pa`,
`esp32s3-qspi`, `tbeam`, `tbeam-supreme`, `heltec-ws`, `heltec-wb`,
`heltec-wp`, `heltec-v3`, `heltec-v4`, `t-deck` and `thinknode-m9`. CI and the release matrix build all of them — both
matrices read `boards.json` rather than a list of their own, so a board added
to the registry is built without touching a workflow. A change therefore has
to compile everywhere, including the boards with no SD slot, where `HAS_SD 0`
has to actually work. Add `-D` overrides under `build_flags`.

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

### Does duty-cycled receive drop anything
`radio.rx_duty_cycle` sleeps the SX1262 between preamble samples. It ships
defaulted off, and no power profile may arm it until there is bench evidence
that a sleeping receiver hears what a listening one does.
`tools/hil_frames.py` is the harness that collects that evidence: it transmits
sequence-numbered frames through Reticulum and reads the node's LoRa receive
counter over the console between each one, so a loss is attributed to a
*particular* frame rather than subtracted from a total at the end. It runs the
same frames with the setting off and on, interleaved in blocks, and compares
the two.

The comparison is the point. "Zero missed packets across 10 000 frames" asks an
RF link for a 0.00 % raw loss rate, which no over-the-air link delivers, so a
run that failed it could not distinguish a mis-sized sleep window from an
ordinary afternoon. Against an interleaved control it can.

It is a bench tool, not a CI job: it needs a node, an RNode and an afternoon,
and `hil.yml` does not run it. What CI does hold is
`tools/tests/test_hil_frames.py` — the arithmetic, the pacing against the
sub-band's transmit allowance, the config it writes — and, because CI installs
`rns`, the one assertion that pins its hand-copied `RNS_HEADER_BYTES` to the
header Reticulum actually packs. See
[optional_deps.py](../tools/tests/optional_deps.py) for why that one must never
be allowed to skip.

## Soak testing
A soak is only worth running if someone reads the result, and a week of JSON is
not something anyone reads. `tools/soak.py` samples every node by its mDNS name
and summarises what a soak is actually asked:

```sh
python tools/soak.py --out soak.csv retimesh-8249cc retimesh-cd5a28   # collect
python tools/soak.py --summarise soak.csv                             # read it
```

A soak is only as good as the build under it. Level every node in the fleet —
firmware *and* filesystem, since `-t upload` writes only the application —
and check `origin/main` immediately before flashing rather than when the
branch merged: a run started on a commit that has since moved is measuring
firmware nobody has any more, which is what voided the 2026-08-27 run's
conclusions. Record the commit beside the CSV; the node cannot tell you,
because a bench build reports `version=dev`.

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


### Watching a fleet over the air
`tools/soak.py` reaches a node over HTTP, which needs Wi-Fi — and a node being
measured is often a node on battery with its Wi-Fi turned down or off by the
very profile under test, or on a hill where there is no Wi-Fi to reach. Two
tools ask the same questions over LXMF instead, so they work wherever the mesh
does:

* [`tools/soak/`](../tools/soak/README.md) keeps every answer in
  newline-delimited JSON. The right thing when the point is to not lose a
  reading.
* [`tools/metrics/`](../tools/metrics/README.md) keeps only the last answer and
  serves it to Prometheus, with a `docker compose up` that brings a Grafana and
  the fleet dashboard with it. The right thing when the point is to watch.

Both share `tools/lxmf_wire.py` — the sensor ids, fixed-point scales and array
positions from `src/rns/Telemetry.h`, in Python, which no compiler checks
against the original. `tools/tests/test_lxmf_wire.py` pins it to documents the
firmware's own encoder produced, because the way telemetry drift shows up is
not an error: a sensor that was added is quietly missing from a dashboard, or a
scale that moved reads as a node that walked half a degree east.

Telemetry needs no enrolment. The console channel — `STACKS`, `POWER` — needs
the collector's address in `maintenance.rns_admins`, and without it every
request is answered `RM ERR ADMIN 403`.

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
cost: http + dns         +22028 B  (118764 free, 110580 largest, +95080 B since boot)
cost: mdns                +6368 B  (112396 free, 110580 largest, +101448 B since boot)
cost: local links         +0 B  (102912 free, 102388 largest, +112692 B since boot)
cost: lora radio       +1216 B  (101696 free,  98292 largest, +113908 B since boot)
cost: reticulum       +52668 B  ( 49028 free,  45044 largest, +166576 B since boot)
cost: rns tcp server    +272 B  ( 48756 free,  45044 largest, +166848 B since boot)
cost: autointerface    +9188 B  ( 39568 free,  36852 largest, +176036 B since boot)
cost: tasks           +35020 B  (  4548 free,   4084 largest, +211056 B since boot)
```

That is a real Heltec Wireless Stick, before the fix below, and the last line
is what it had left to run on: **4548 bytes**. It was not enough. That board
threw `std::bad_alloc` out of the Reticulum loop within seconds of finishing
boot and panicked, and its `boot_count` had run to 181. Nothing else the node
reported said so — `heap_free` read a comfortable 46 KB, because most of what
was left is 32-bit-only IRAM that no allocation can use.

Read the rest as the answer to "which switch is worth making cost nothing when
it is off", per board. Wi-Fi is billed in two parts on purpose: the **radio**
is what the switch is meant to buy, and **http + dns** plus **mdns** is what a node
pays whether Wi-Fi is on or off — so on this board, switching Wi-Fi off saves
52 KB and still leaves 28 KB on the table for a web server nobody can reach.
That, and not USB-NCM, is where lazy allocation is worth the work.

It also found a bug outright, which is the point of measuring. `packet rings`
was costing 26 916 B of scarce internal RAM: `psramRing()` falls back to the
internal heap on a board with no PSRAM, the fallback said nothing, the three
ring sizes were chosen as though PSRAM would absorb them, and the fallback path
leaked its control block every time. Boards without PSRAM now take 4096 B per
ring rather than 8192 (`RING_BYTES` in `Config.h`, overridable per board), the
leak is gone, and the boot log names where the storage came from. On the Stick
that is 13 720 B instead of 26 916, and it ends boot with 17 428 B free instead
of 4548 — the difference between a board that panicked twice in seventy seconds
and one that ran ten minutes with no panic and no `bad_alloc`.

It is not the difference between broken and well, and it is worth being exact
about that: over those ten minutes the same board's low-water mark still fell
to **956 bytes**, and it still aborted once under load. Twelve kilobytes back
buys a board this tight some room, not health. What it needs next is the 28 KB
web server made lazy, and probably more after that.

The trade is deliberate: 4096 B holds about eight RNS packets, which is seconds
of backlog at LoRa speeds, and a ring that turns out to be too small says so
rather than hiding it — the send fails, `LoRa TX ring full` is logged, and
`lora_rx_drop_ring` counts it in `/api/status`.

And `local links` reads 0 on this board but 10 556 B on a Heltec V3 with
`links.ppp` on — the PPP interface and its reader task, which is what that
switch now gives back.

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
| `test_hdlc` | RNS TCP framing, mostly its malformed half — truncation, dangling escapes, garbage, the MTU boundary — each corruption case ending by proving the next valid frame still decodes |
| `test_hdlc_fuzz` | the same deframer against a seeded corpus nobody chose, held to properties rather than to expected bytes (see below) |
| `test_airtime` | duty cycle, dwell budget, CSMA accounting |
| `test_radio_plan` | per-chip radio limits, regional regimes, node naming |
| `test_store_home` | where the Reticulum store belongs, card ownership, what a move does |
| `test_local_link` | the local-link phase machine and the host-facing trust rule |
| `test_bootloader` | which bootloader methods a board offers, the restart sequence and its re-arm rule |
| `test_maintenance` | the console protocol: parsing, malformed and overlong lines, noise, replies, `AUTH` |
| `test_settings_rules` | what a settings value may be, held to the vectors the API and the console share |
| `test_ppp_uart` | who owns the bridge UART: what takes the port for PPP and, more importantly, what must not |
| `test_display_refresh` | whether a drawn frame is worth pushing to the glass, and how |
| `test_version_label` | what the status row shows for a version too long to print whole |
| `test_lxmf` | the LXMF wire format both ways, and above all what the parser refuses — these bytes arrive from whoever is in earshot |
| `test_lxmf_vectors` | the same parser against messages and announces the reference library actually produced (see below) |
| `test_lxmf_inbox` | the inbox record format and the ring arithmetic: whether a message read back is the message that was stored |
| `test_rns_admin` | who may command a node — each test is a way in that must stay shut |
| `test_lxmf_commands` | what the node says back to a ping, an echo and a signal report, and that nothing a stranger sends can overrun the reply |
| `test_telemetry` | what the node says about itself: sensor shapes, msgpack str against bin, and that a document too big is not sent half-written |
| `test_nomadnet` | the node's own page: what it says, what it refuses to claim, and that a page never overruns the buffer a stranger asked it to fill |

### HDLC fuzzing and the sanitizer gate

`test_hdlc` names its corruptions: every case in it is there because somebody
thought of it. `test_hdlc_fuzz` is for the ones nobody thought of. Port 4242 is
reachable by anything on the network, so the deframer does not get to choose its
input. The suite generates streams from a splitmix32 seeded at `kDefaultSeed`
(`0x7E7D5E5D`) and asserts properties rather than bytes: recovery after any
prefix at all, the MTU bound in both directions, what `oversized()` may and may
not count, that one byte never both delivers a frame and counts a drop, and what
`reset()` clears against what it deliberately keeps.

Nothing in it reads a clock or asks for entropy. Each stream is seeded from
(seed, generator, iteration), so iteration 4711 is the same bytes whatever
budget the run uses, and every failure prints the seed, generator and iteration
that reproduce it exactly.

CI pays for `kDefaultIterations` — 15 000 streams per generator. The fuzz binary
itself runs in about 1.8 seconds; PlatformIO reports the whole `test_hdlc_fuzz`
step at about three, and that is the number to budget CI time against. The
budget and the seed are `argv[1]` and `argv[2]`, so a local run goes far past
CI's without editing anything:

```sh
pio test -e native -f test_hdlc_fuzz                            # the CI budget
pio test -e native -f test_hdlc_fuzz --program-arg 500000       # 500k per generator
pio test -e native -f test_hdlc_fuzz --program-arg 500000 --program-arg 0xC0FFEE01
```

Changing `kDefaultSeed` moves the corpus for everyone, so that is a reviewable
edit rather than something to try locally.

The acceptance criterion is that no input reaches memory outside the parser, and
the guard bands the harness puts either side of the deframer cannot show that on
their own. The overflow a regression here would produce writes one past the end
of `Deframer::buf`, which lands in the object's own alignment padding — still
inside the object — so no band moves and no ASan redzone is crossed either:
**ASan issues no memory-error report for it.**

That is not to say such a regression reaches CI. A parser that writes one byte
too many also *delivers* one byte too many, and the fuzz suite's own MTU oracle
refuses the 501-byte frame, so plain `pio test -e native` fails on it — in all
three stream generators:

```
test/test_hdlc_fuzz/test_main.cpp:<line>:test_fuzz_uniform_random_bytes:FAIL: the
deframer emitted a frame of 501 bytes, over the MTU  [seed=0x7E7D5E5D
gen=0(random-bytes) iter=47 byte=2246]  reproduce: run this test binary with
args '15000 0x7E7D5E5D'
```

`<line>` stands where the tool prints a source position, elided here and in the
sanitizer sample below because nothing regenerates these transcripts: a line
number written into one goes stale the first time anything above it moves. The
rest of each is the tool's own output, the first wrapped here to fit.

What the oracle cannot do is name the write. It sees the frame that came out,
not the byte that went in, and it only sees anything at all when the overrun
reaches a delivered frame. **UBSan's `array-bounds` is what names the write
itself**, which is why the gate is address *and* undefined. The
`native_sanitize` environment extends `native` with
`-fsanitize=address,undefined -fno-sanitize-recover=all` and
`test_filter = test_hdlc*`, so it covers both HDLC suites and nothing else:

```sh
pio test -e native_sanitize -v
```

Use `-v`. PlatformIO's Unity reader drops every line it cannot parse as a test
result, so without it a sanitizer abort reads only as `Program received signal
SIGHUP`, with no reason attached. With it, the reason. A regressed bounds check
would report:

```
src/net/HDLC.h:<line>:<col>: runtime error: index 500 out of bounds for type 'unsigned char [500]'
```

That line is the whole of the program's output: `-fno-sanitize-recover=all`
aborts at the write, before the first frame is emitted and before Unity's
results are flushed. The write is what gets named here, not the frame it would
have gone on to produce.

It is deliberately not wired into CI: the `tests` job runs plain
`pio test -e native`, which already fails on a regressed bounds check as above,
and a sanitizer step failing on a runner for toolchain reasons would cost more
than this gate is worth. What the sanitizer run adds is the write's own name,
and cover for an overrun whose bytes never reach a frame the oracles could
refuse. Run it by hand whenever `HDLC.h` changes.

### LXMF vectors

`test_lxmf` builds its buffers by hand, so it tests what the parser refuses but
only ever offers it shapes we already believed in. Two defects lived under a
green suite because of that — a message carrying a stamp, and one sent as a
single packet — and both were found on the bench instead.

`test_lxmf_vectors` uses bytes generated by LXMF itself. Each vector carries the
message as it arrives at the delivery destination and the `hashed_part` the
library computed over it; reproducing those bytes is the whole of
interoperability, since the signature is taken over `hashed_part` and its own
hash. Comparing the bytes rather than checking the signature is what lets it run
natively, with no crypto linked in. The announce vectors do the same for the
other direction — that is what catches an announce a client cannot read.

Regenerate when LXMF changes the wire format, and read the diff when you do: a
change there is a change in what clients send us. The keys, clock and stamps are
fixed, so an unchanged format regenerates byte for byte.

```sh
pip install rns lxmf
python tools/lxmf_vectors.py > test/test_lxmf_vectors/vectors.h
```

### Telemetry shapes

A telemetry value in the wrong msgpack type or the wrong shape is not rendered
wrongly by a client — it is dropped, so "it looked right in the hex" is not
evidence. `test_telemetry` takes a document apart with this node's own decoder
and holds the shapes; `tools/telemetry_check.py` goes further and asks the
class that will actually read it. Run it when a sensor is added or its shape
changes:

```sh
pip download sbapp --no-deps --no-binary :all: -d /tmp/sb
tar -xzf /tmp/sb/sbapp-*.tar.gz -C /tmp/sb
python tools/telemetry_check.py /tmp/sb/sbapp-1.8.0
```

It prints what each sensor reads back as, for a board with a fix and a charger
it can see and for one with neither. A sensor missing from that output is one
the app silently dropped.

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
partitions/     app and A/B layouts per flash size (ota_4mb, ota_8mb, ota_16mb,
                huge_app_8mb); each file carries its own arithmetic
tools/          make_manifest.py (release bundles), build_site.py (Pages),
                bump_deps.py (PlatformIO dependency PRs), retimesh-flash/ (CLI
                and the shared device/bootloader module), hil.py and
                hil_bootloader.py (hardware-in-the-loop), soak.py (fleet
                sampler and summariser), asset_stamp.py (build-time web asset
                hash), board_caps.py (boards.json -> BOARD_* flags),
                check_boards.py (boards.json consistency, CI),
                console.py (the maintenance console over a port or a socket),
                hil_ppp.py (PPP hardware-in-the-loop),
                hil_frames.py (duty-cycled receive frame-loss harness, bench),
                hilreport.py (HIL results as a job summary),
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
