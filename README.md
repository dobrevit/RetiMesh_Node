# RetiMesh Node

[![CI](https://github.com/dobrevit/RetiMesh_Node/actions/workflows/ci.yml/badge.svg)](https://github.com/dobrevit/RetiMesh_Node/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/dobrevit/RetiMesh_Node?include_prereleases)](https://github.com/dobrevit/RetiMesh_Node/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

Standalone **Reticulum transport node** firmware for ESP32-S3 boards with an
SX1262 or SX1276/78 LoRa transceiver (LilyGO T3-S3 and friends — the chip
is detected at boot, one build covers both variants).

📖 **Documentation:** [docs/](docs/README.md) — getting started, architecture,
configuration, Reticulum integration, hardware, HTTP API, examples,
troubleshooting, development. The node runs a Wi-Fi access point with a
captive-portal status page and bridges stock Reticulum clients (Sideband,
`rnsd`, MeshChat) onto a LoRa channel — no host computer, no internet.

```
  phone / laptop (RNS client)          RetiMesh Node               LoRa channel
 ───────────────────────────────────────────────────────────────────────────────
  Sideband ── Wi-Fi ── TCP :4242 ──►  HDLC deframe ─► txRing ─► CSMA+TX ──► RF
  Sideband ◄─ Wi-Fi ◄─ TCP :4242 ◄──  HDLC frame  ◄─ rxRing ◄─ reassemble ◄─ RF
  browser  ── Wi-Fi ── HTTP :80  ──►  status page + public bulletin board
```

## What it is (and is not)

- **Transparent bridge.** Raw RNS packets pass through unmodified in both
  directions. The node never parses, decrypts or routes packet contents —
  end-to-end encryption stays entirely between the Reticulum peers, using
  their own keys.
- **A Reticulum Transport node.** The firmware embeds
  [microReticulum](https://github.com/attermann/microReticulum) (Apache-2.0),
  a C++ port of RNS, with transport enabled: it keeps a path table, propagates
  announces, answers path requests and forwards packets hop by hop — exactly
  `rnsd` with `enable_transport = yes`. Each interface has its own **mode**
  (`full`, `gateway`, `access_point`, `roaming`, `boundary` — rnsd's
  vocabulary), set on the settings page: the LoRa channel defaults to `full`,
  Wi-Fi clients to `access_point` so phones never receive the announce
  flood. Transport can be disabled to fall back to a plain bridge.
- **RNode-compatible RF layer.** The LoRa framing (1-byte header with
  sequence nibble + split flag, ≤255-byte frames, two-fragment packets up
  to RNS's 500-byte MTU) is byte-identical to
  [RNode_Firmware](https://github.com/markqvist/RNode_Firmware), so real
  RNodes on the same channel parameters interoperate with this gateway.

## Interfaces

| Port | Protocol | Purpose |
|------|----------|---------|
| — | Wi-Fi SoftAP `retimesh-XXXXXX` (last three MAC octets), `10.42.0.1/24` | open network, captive portal DNS |
| 80 | HTTP (ESPAsyncWebServer) | status page, neighbour list, **unencrypted** community bulletin board; `/settings.html` admin page (user `admin`, default password **`retimesh`** — change it there) |
| 4242 | raw TCP, RNS HDLC framing | Reticulum transport — connect any stock RNS client |
| — | USB serial, 115200 | the log, and a maintenance console (`VERSION`, `STATUS`, `BOOTLOADER CONFIRM`, …) — see [docs/local-link.md](docs/local-link.md) |

Both servers bind every interface, so they answer on whichever local link is
up — Wi-Fi today, USB networking and PPP once a toolchain with those drivers
lands — and Wi-Fi can be switched off entirely.

Client-side config (`~/.reticulum/config` — in Sideband just add a
*TCP Client Interface* with the same host/port):

```ini
[[RetiMesh Gateway]]
  type = TCPClientInterface
  enabled = yes
  target_host = 10.42.0.1
  target_port = 4242
```

## Installing a release

- **Browser (easiest):** open the [web flasher](https://dobrevit.github.io/RetiMesh_Node/),
  pick your board, click *Install* (Chrome/Edge, Web Serial).
- **Terminal:**
  ```sh
  pipx run --spec "git+https://github.com/dobrevit/RetiMesh_Node#subdirectory=tools/retimesh-flash" retimesh-flash install
  ```
  Interactive board/port selection, checksum verification, `--mode app` to
  keep settings, `--mode fs` for the web app only — see
  [tools/retimesh-flash](tools/retimesh-flash/README.md).
- **Manual:** every release ships `retimesh-node-<ver>-<board>-merged.bin`
  (flash at `0x0` with esptool) plus a zip with the individual partitions;
  offsets and SHA-256 hashes are in the release's `release.json`.

## Building from source

```sh
pio run -e t3s3                  # compile (LilyGO T3-S3)          
pio run -e t3s3 -t upload        # flash firmware — asks the running node for its bootloader, no BOOT button
pio run -e t3s3 -t uploadfs      # flash the web app (data/ -> LittleFS)
pio device monitor
```

`t3s3` matches LilyGO's own board definition for the T3-S3 v1.2/v1.3
(ESP32-S3FH4R2: 4 MB flash, 2 MB in-package quad PSRAM → `qio_qspi`,
`huge_app.csv` partitions). `esp32s3-qspi` targets a generic 8 MB
DevKitC-1 with an external SX1262 — override the `PIN_LORA_*` build flags
there to match your wiring. If `esptool.py flash_id` reports a 16 MB
N16R8 module instead, switch that env to `qio_opi` / `16MB`.

All tunables (pins, RF parameters, SSID, ports, buffer sizes) live in
[src/Config.h](src/Config.h) and can be overridden per-environment with
`-D` build flags.

**Radio parameters must match every node on the channel** — frequency,
bandwidth, spreading factor, coding rate *and* sync word (`0x12`). Defaults
are 868.100 MHz, BW 125 kHz, SF8, CR 4/5, 7 dBm; change them at runtime on
the settings page (the page prints the matching `rnsd` `RNodeInterface`
block). Check your local regulations before changing frequency or power.

## Discovery: announces, beacons, station IDs

The node has a persistent **Reticulum identity** (X25519 + Ed25519 keys in
NVS, kept across settings resets) and a `retimesh.node` destination. It
**announces** it on boot and every `announce_interval` seconds (default 10
min; 0 = off) — on LoRa and to connected Wi-Fi clients — so every RNS peer
learns a path to it (`rnpath -t` lists it, `rnstatus` counts it). Announces
heard from either side are parsed, **signature-verified** and listed as
neighbours with aspect (`lxmf.delivery` = Sideband/LXMF peers,
`nomadnetwork.node`, `retimesh.node`, …), hop count, display name and signal.
That is the normal Reticulum way to see who is on the mesh, and it costs no
protocol violations anywhere. `/api/status` exposes the node's `identity`
and `destination` hashes.

**Beacons** (`beacon_interval`, default 0 = off) are a RetiMesh-only quick
probe: `RM1 I <callsign> <version>` after that many seconds of TX silence,
a hello (`RM1 H …`) on boot answered by other RetiMesh nodes (`RM1 R …`)
within seconds. Everything heard is on the status page and counted on the
OLED (`NB`).

Beacons are **valid Reticulum packets**: a broadcast to the PLAIN destination
`retimesh.beacon`. RNS peers parse and silently drop them (no protocol
violation, never forwarded past one hop), and any RNS program can receive
them:

```python
import RNS, time
RNS.Reticulum()
d = RNS.Destination(None, RNS.Destination.IN, RNS.Destination.PLAIN, "retimesh", "beacon")
d.set_packet_callback(lambda data, packet: print(packet.receiving_interface, data.decode()))
while True: time.sleep(1)
```

The node also lists RNode **station IDs** — the raw callsign an RNS
`RNodeInterface` transmits — so an RNode with

```ini
  id_interval = 45
  id_callsign = MYCALL
```

(note: `beacon`/`beacon_interval` are not RNS config keys — `id_*` are)
appears in the neighbour list too. The callsign defaults to the SSID.

## Architecture

- **Core 0** — Wi-Fi/LwIP stack, the single AsyncTCP event task (socket
  I/O for ports 80 *and* 4242) and the captive-portal DNS poller.
- **Core 1** — `radioTask` (SX1262 IRQ service, CSMA, RNode framing) and
  `bridgeTask` (LoRa→TCP fan-out). Radio timing is never blocked by web
  or Wi-Fi work.
- Two FreeRTOS **ring buffers** (`RINGBUF_TYPE_NOSPLIT`, one item = one
  RNS packet) bridge the cores; both directions drop on overflow rather
  than block, because a stalled radio task is worse than a lost packet
  (Reticulum links tolerate loss).

Per-file tour: [main.cpp](src/main.cpp) (task layout, ring buffers) ·
[WifiManager](src/WifiManager.cpp) (AP, captive portal, web API) ·
[RetiTransportServer](src/RetiTransportServer.cpp) (port 4242, HDLC, hub
relay) · [LoRaRadio](src/LoRaRadio.cpp) (RadioLib SX1262, CSMA,
fragmentation) · [HDLC.h](src/HDLC.h) (RNS TCP wire framing).

## Development workflow

| Workflow | Trigger | What it does |
|---|---|---|
| [CI](.github/workflows/ci.yml) | push to `main`, PRs | builds every board (`boards.json`), builds the LittleFS image, packages bundles as artifacts, reports flash/RAM in the job summary, smoke-tests the CLI |
| [Release Drafter](.github/workflows/release-drafter.yml) | merges to `main`, PR events | maintains a draft release with categorised notes; auto-labels PRs from branch names / conventional-commit titles |
| [Release](.github/workflows/release.yml) | tag `vX.Y.Z` | builds all boards with `FW_VERSION` = tag, attaches per-board zips, merged images, `release.json` and `sha256sums.txt` to the draft |
| [Web flasher](.github/workflows/pages.yml) | release published | rebuilds the GitHub Pages site from the last 5 published releases |
| [PlatformIO deps](.github/workflows/pio-deps.yml) | monthly | Dependabot stand-in for `lib_deps`: opens a PR bumping library versions from the PlatformIO registry |
| Dependabot | weekly | GitHub Actions versions and the CLI's Python deps |

Cutting a release: merge PRs (labels drive the version: `major`/`breaking`,
`minor`/`feature`, everything else → patch) → check the draft → push the tag
it names (`git tag v1.2.0 && git push origin v1.2.0`) → wait for assets →
**Publish**. Publishing deploys the web flasher.

One-time repo setup: *Settings → Pages → Source: GitHub Actions*; optionally
a `DEPS_PR_TOKEN` fine-grained PAT so dependency PRs trigger CI.

Adding a board = a `[env:…]` in `platformio.ini`, an entry in `boards.json`
(including its `local_link` block — what is on the USB connector), and the
env name in the two workflow matrices.

## Notes & limits

- **Settings** (radio channel, access point, admin password) live in NVS and
  are edited at `http://10.42.0.1/settings.html` (user `admin`, default
  password `retimesh` — change it). Radio changes apply live; Wi-Fi changes
  restart the node.
- **USB networking (CDC-NCM) and PPP are designed and tested down to the
  toolchain and no further.** The pinned Arduino core 2.0.17 ships TinyUSB
  without the NCM class and lwIP without PPP; both arrive with the core-3
  migration. What works today on every board: the maintenance console, the
  bootloader manager, `POST /api/system/bootloader`, Wi-Fi-optional
  operation and hands-free flashing. [docs/local-link.md](docs/local-link.md)
  has the full account.
- **WPA3 on the access point is not available on this build.** SoftAP-side
  SAE requires ESP-IDF 5; the pinned Arduino core (2.0.17 / IDF 4.4.7)
  rejects the mode, so the node runs WPA2 and greys out the WPA3 options.
  The code path is in place for a core-3 migration.

- Max `RNS_MAX_CLIENTS` (4) simultaneous TCP peers; slow consumers get
  packets dropped, not queued forever.
- The bulletin board is deliberately public/plaintext and local to the
  node (no RTC → posts are ordered, not timestamped). Capped at 50 posts,
  rotated oldest-first.
- The captive portal serves HTTP only; modern OSes show the "sign in to
  network" sheet via their connectivity probes, all of which redirect to
  `http://10.42.0.1/`.
- CSMA is a simplified listen-before-talk (random slotted backoff + CAD
  probe), not RNode's full DIFS/contention-window machine.

## License

Copyright © 2026 Dobrev IT Ltd. RetiMesh Node is free software, released
under the **GNU General Public License v3.0 or later** — see
[LICENSE](LICENSE). If you distribute or modify it, you must adhere to the
GPLv3: provide the corresponding source, keep copyright and license notices,
and inform users of their rights.

Third-party components keep their own licenses: microReticulum, microStore
and their Crypto fork (Apache-2.0 / MIT), RadioLib (MIT), ArduinoJson (MIT),
MsgPack (MIT), Adafruit GFX/SSD1306/BusIO (BSD/MIT), ESPAsyncWebServer and
AsyncTCP (LGPL-3.0). The LoRa wire format is
implemented for interoperability with [RNode_Firmware](https://github.com/markqvist/RNode_Firmware)
(GPLv3, © Mark Qvist); no RNode source code is included.
