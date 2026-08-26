# Hardware

## Supported boards
| Env | Board | Radio | Display | Extras | Status |
|---|---|---|---|---|---|
| `t3s3` | LilyGO T3-S3 v1.2/v1.3 (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM) | SX1276/78 **or** SX1262 — detected at boot | 0.96" SSD1306 (I²C) | microSD, battery ADC | verified (SX1276), SX1262 expected |
| `tbeam` | LilyGO T-Beam v1.1/v1.2 (ESP32, 4 MB flash, no PSRAM) | SX1276 (v1.1) **or** SX1262 (v1.2) — detected at boot | 0.96" SSD1306 (I²C) | 18650 holder, AXP192/AXP2101 PMU, u-blox GPS; **no SD slot** | see below |
| `esp32s3-qspi` | ESP32-S3 DevKitC-1 (8 MB) + SX1262 module | SX1262 | optional SSD1306 | — | builds; wire per flags |

### T-Beam notes
The transceiver, the GPS and the display are not wired to 3V3 on this board:
each hangs off a regulator inside the power-management chip, and they come up
*off*. The firmware brings the PMU up before probing the radio — v1.1 carries
an AXP192, v1.2 an AXP2101, both at I²C `0x34` and told apart by their chip id,
so one build covers either revision.

The GNSS receiver is powered from that rail and switched on by default
(*GNSS receiver* on the settings page turns it off to save tens of
milliamps). It gives the node two things: a position, and — more useful — a
real clock. A LoRa node has no RTC, so its idea of "now" restarts at zero on
every reboot; a receiver with a fix carries proper UTC, which the node adopts
for its system clock and re-checks hourly. Log lines and the SD event log
become meaningful across restarts.

Sentences are parsed on the device (RMC and GGA, checksum-verified,
talker-agnostic so GP/GN/GL/GA all work) — about a hundred lines, no library.
The display gains a **GNSS page** showing fix state, satellite count,
position, altitude and UTC. `/api/status` reports the receiver's health under
`gps`, but not where the node is: that endpoint needs no credentials, so
coordinates are held back unless the caller logs in as the admin or the
operator publishes them from the settings page.
Indoors expect `searching, 0 sats` with the sentence counter climbing: that
tells you the receiver is wired and talking, and only the sky view is
missing.

Without a card slot the Reticulum store lives in the flash partition (~900 KB
shared with the web app), so `transport.sd_store` has no effect here.

Battery voltage, charge state and percentage come from the PMU rather than an
ADC divider, which is why this board can say whether a cell is actually
connected and whether it is charging.

| Function | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI / CS / RST | 5 / 19 / 27 / 18 / 23 |
| LoRa DIO0 (SX1276) | 26 |
| LoRa DIO1 / BUSY (SX1262) | 33 / 32 |
| OLED + PMU I²C (SDA / SCL) | 21 / 22 |
| PMU IRQ | 35 |
| GPS RX / TX | 34 / 12 |
| User button | 38 |

## T3-S3 pin map (defaults in `Config.h`)
| Function | GPIO |
|---|---|
| LoRa SCK / MISO / MOSI / CS | 5 / 3 / 6 / 7 |
| LoRa RST | 8 |
| SX1262: DIO1 / BUSY | 33 / 34 (DIO2 = RF switch, TCXO 1.8 V) |
| SX127x: DIO0 / DIO1 | 9 / 33 |
| OLED SDA / SCL | 18 / 17 (addr 0x3C) |
| BOOT button | 0 (active low) |
| LED | 37 |
| Battery ADC | 1 (100k/100k divider) — not yet used |
| microSD MOSI / MISO / SCK / CS | 11 / 2 / 14 / 13 (HSPI, separate from the radio bus) |

The SX1262 probe waits on BUSY (GPIO 34 = DIO2 on SX127x boards) and takes
~27 s to fail, so the SX127x is probed first (~100 ms to fail on SX1262).

## microSD card
Optional; hot-plug polled every 3 s. The card is mounted as one FAT volume at
`/sd`. Status values: `mounted`, `partial` (the FAT volume covers less than
half the card — e.g. a Raspberry Pi image with a small boot partition),
`unformatted` (no filesystem the node recognises), `formatting`, `error`,
`absent`. The settings page can format the whole card to a single FAT32
volume (admin, explicit confirmation — erases everything). First consumer:
`/retimesh/events.log` (announces, boots; rotated at 1 MB). Transport
persistence and the propagation-node store move to the card in later
releases.

## OLED and button
Pages: status → neighbours → transport (interfaces, modes, traffic, path
count) → radio → network (dots bottom-right). Short press:
next page (wakes the panel first if asleep); long press (1.5 s): blank/wake.
Panel sleeps after 60 s without a press; page returns to status after 30 s.

Pages: status, neighbours, transport, radio, network, QR. The QR page shows a
scan-to-join code for the access point.

## Adding a board
1. `src/boards/<name>.h`: the pin map and the capability flags (`HAS_SD`,
   `HAS_PMU`, `HAS_GPS`, `HAS_DISPLAY`, `HAS_BATTERY_ADC`, `BOARD_NAME`).
   Everything in `Config.h` is `#ifndef`-guarded, so the board header wins and
   anything it omits falls back to a sensible default.
2. `src/Config.h`: one line in the board-selection block mapping `-DBOARD_<X>`
   to the header.
3. `platformio.ini`: a new `[env:<name>]` (board, partitions, `-DBOARD_<X>`,
   and `build_unflags` for anything the base env sets that the board lacks —
   PSRAM and native-USB CDC are the usual ones).
4. `boards.json`: name, chip family, notes — drives CI, release packaging, the
   web flasher and the CLI.
5. Workflow matrices in `.github/workflows/ci.yml` and `release.yml`.
6. If the display or radio differ: `Display.*` / `LoRaRadio.*` (probe order,
   TCXO, RF switch). Keep board specifics behind the capability flags.
7. Verify: boot log clean, radio detected, announce accepted by an RNS peer.

## Flashing details
Offsets (from the env's partition table): bootloader `0x0`, partitions
`0x8000`, boot_app0 `0xE000`, app `0x10000`, LittleFS at the `spiffs`
partition (`0x310000` on the 4 MB layout). Each release ships per-partition
files, a merged image for `0x0`, `manifest.json` (ESP Web Tools) and
`release.json` with offsets and SHA-256 hashes.
