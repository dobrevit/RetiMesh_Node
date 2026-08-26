# RetiMesh Node

[![CI](https://github.com/dobrevit/RetiMesh_Node/actions/workflows/ci.yml/badge.svg)](https://github.com/dobrevit/RetiMesh_Node/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/dobrevit/RetiMesh_Node?include_prereleases)](https://github.com/dobrevit/RetiMesh_Node/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

Standalone **Reticulum LoRa gateway** firmware for ESP32-S3 boards with an
SX1262 (LilyGO T3-S3 and friends). The node runs a Wi-Fi access point with a
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
- **Not an RNS Transport instance.** Routing intelligence (announces, path
  requests, retransmission) lives in the connected clients' RNS stacks.
  Enable *Transport* on one of the connected peers if you want this hop to
  route for a wider network.
- **RNode-compatible RF layer.** The LoRa framing (1-byte header with
  sequence nibble + split flag, ≤255-byte frames, two-fragment packets up
  to RNS's 500-byte MTU) is byte-identical to
  [RNode_Firmware](https://github.com/markqvist/RNode_Firmware), so real
  RNodes on the same channel parameters interoperate with this gateway.

## Interfaces

| Port | Protocol | Purpose |
|------|----------|---------|
| — | Wi-Fi SoftAP `RetiMesh-Node`, `10.42.0.1/24` | open network, captive portal DNS |
| 80 | HTTP (ESPAsyncWebServer) | single-page status app + **unencrypted** community bulletin board (LittleFS) |
| 4242 | raw TCP, RNS HDLC framing | Reticulum transport — connect any stock RNS client |

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
pio run -e t3s3 -t upload        # flash firmware
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
bandwidth, spreading factor, coding rate *and* sync word (`0x12`). For a
real RNode peer, configure its RNS interface with the same values. Check
your local regulations before changing `RF_FREQ_MHZ` / `RF_TX_DBM`
(default: 869.525 MHz, EU868 SRD).

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

Adding a board = a `[env:…]` in `platformio.ini`, an entry in `boards.json`,
and the env name in the two workflow matrices.

## Notes & limits

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

Third-party components keep their own licenses: RadioLib (MIT), ArduinoJson
(MIT), ESPAsyncWebServer and AsyncTCP (LGPL-3.0). The LoRa wire format is
implemented for interoperability with [RNode_Firmware](https://github.com/markqvist/RNode_Firmware)
(GPLv3, © Mark Qvist); no RNode source code is included.
