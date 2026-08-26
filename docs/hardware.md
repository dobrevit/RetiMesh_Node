# Hardware

## Supported boards (v0.0.3)
| Env | Board | Radio | Display | Status |
|---|---|---|---|---|
| `t3s3` | LilyGO T3-S3 v1.2/v1.3 (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM) | SX1276/78 **or** SX1262 — detected at boot | 0.96" SSD1306 (I²C) | verified (SX1276), SX1262 expected |
| `esp32s3-qspi` | ESP32-S3 DevKitC-1 (8 MB) + SX1262 module | SX1262 | optional SSD1306 | builds; wire per flags |

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
Pages: status → neighbours → radio → network (dots bottom-right). Short press:
next page (wakes the panel first if asleep); long press (1.5 s): blank/wake.
Panel sleeps after 60 s without a press; page returns to status after 30 s.

## Adding a board
1. `platformio.ini`: a new `[env:<name>]` (memory type, flash size,
   partitions, `PIN_*` overrides).
2. `boards.json`: name, chip family, notes — drives CI, release packaging, the
   web flasher and the CLI.
3. Workflow matrices in `.github/workflows/ci.yml` and `release.yml`.
4. If the display or radio differ: `Display.*` / `LoRaRadio.*` (probe order,
   TCXO, RF switch). Keep board specifics behind build flags.
5. Verify: boot log clean, radio detected, announce accepted by an RNS peer.

## Flashing details
Offsets (from the env's partition table): bootloader `0x0`, partitions
`0x8000`, boot_app0 `0xE000`, app `0x10000`, LittleFS at the `spiffs`
partition (`0x310000` on the 4 MB layout). Each release ships per-partition
files, a merged image for `0x0`, `manifest.json` (ESP Web Tools) and
`release.json` with offsets and SHA-256 hashes.
