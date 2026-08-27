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

Charging is set up at boot — 4.2 V target, up to 500 mA (450 mA on an AXP192)
— and the board's indicator LED is left under the charger's control, so it
blinks while current is going into the cell and settles when it is full. The
display shows `bat 42%+` while charging and the status page says so in words.

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
Pages, in cycle order: status → neighbours → transport → radio → network →
GNSS (where a receiver is fitted) → QR, with the current one marked by the
dots at the bottom right. Short press: next page (wakes the panel first if
asleep); long press (1.5 s): blank/wake. The panel sleeps after 60 s without a
press, and the page returns to status after 30 s.

The QR page shows a scan-to-join code for the access point and uses the whole
panel; every other page has a header carrying the page name on the left and,
where a cell is fitted, a battery icon on the right. The icon fills in six
steps and always shows what the cell actually holds; while charging, one
segment travels up it inverted against the fill, so a cell at 100% still reads
as charging rather than looking identical to a full idle one.

Reading the denser rows:

- **transport** has four rows for interfaces, each `name mode traffic`. Four or
  fewer are listed in full; with more, three are listed and the fourth row
  reads `+N more`, so a live interface is never dropped without saying so. The
  bottom row is `an rx/tx bc rx/tx` — announces and beacons, received over
  sent. Counters past 999 are shown in thousands (`12k`) so the row cannot
  outgrow the 21 columns the panel has.
- **radio** carries the preamble length as `p<n>` and the sync word as
  `sy<hex>` alongside the channel. A node on the wrong sync word hears nothing,
  and that should be visible without opening the web UI.
- **network** shows the AP name and security, then `ch<n> cli <n> rns <n>` —
  the Wi-Fi clients and RNS TCP clients attached. Those stay visible whether or
  not the node has joined an upstream network. The AP address is not repeated
  here; it is the fixed `10.42.0.1` shown on the status page.

Signal strength is deliberately *not* in the header. A bar chart with no label
and no number says nothing, so the meters live on the pages that can explain
them: the radio page shows `sig -87dBm` and `snr 8.5dB` with bars beside each
figure, and the network page shows the Wi-Fi uplink the same way when the node
has joined a network. Each meter is scaled against the window that applies to
it — LoRa RSSI over -135..-75 dBm, Wi-Fi RSSI over -90..-40 dBm, and SNR
against the SX127x demodulation floor for the current spreading factor
(-7.5 dB at SF7 to -20 dB at SF12), because the same SNR means something
different at SF7 and SF12.

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

## 2.4 GHz and the US band

The SX1280 build (`t3s3-sx1280`) drives the same T3-S3 carrier board with a
2.4 GHz module. The radio is chosen at build time, not detected: probing works
by tuning the chip and seeing whether it answers, and an SX1280 will no more
accept an 868 MHz channel than an SX1262 will accept 2445 MHz — whichever
settings the probe carries, one of the two fails for the wrong reason. So the
two images are not interchangeable, and `boards.json` lists them separately.

Bounds now come from the transceiver that is fitted rather than from a sub-GHz
assumption. `/api/status` reports them under `radio.caps`: tuning range, the
bandwidth steps the chip actually has, the spreading-factor and power ranges,
and which band plan the configured channel falls under. The SX1280 offers four
bandwidths — 203.125, 406.25, 812.5 and 1625 kHz — and none of them appear in
the SX127x list, which is why a channel plan copied across from 868 MHz will
not name one this chip can tune.

Three band plans are recognised, and they constrain different things. Treating
them as variations on a duty cycle would misdescribe two of the three:

| Band | What is capped | What the node does |
|---|---|---|
| EU 863-870 MHz | Hourly duty cycle, 0.1 %-10 % by sub-band | Derives the budget from the sub-band and holds to it |
| US 902-928 MHz | How long one transmission may hold a channel — 400 ms for a hopping system | Checks the longest frame against that ceiling and says so if it does not fit |
| 2.4 GHz ISM | Radiated power and listen-before-talk | CSMA, no budget |

The US case is the one worth reading twice. There is no hourly allowance to
spend, so a node there is not "unlimited" — the binding constraint is
per-packet. This firmware does not frequency-hop, so it has one channel, and
the dwell ceiling applies to everything it sends. A full 254-byte fragment at
SF12/125 kHz is far past 400 ms, which is exactly the configuration you arrive
at by copying an EU channel plan across. Either widen the channel to 500 kHz or
more, at which point it qualifies as a digital transmission system and the
dwell limit stops applying, or use a spreading factor whose longest frame fits.
The node logs which of those it is in at boot and warns when the configuration
cannot comply.

None of this is a compliance claim. It is the firmware applying the plan that
matches the band it has been tuned to; what is legal where the node is standing
is the operator's to know.
