# Hardware

## Supported boards
<!-- boards.json:begin -->
<!-- Rendered by tools/board_docs.py from boards.json. Edit the registry, not this table. -->
| Env | Board | MCU | Radio | Display | Extras | Status |
|---|---|---|---|---|---|---|
| `t3s3` | LilyGO T3-S3 v1.2/v1.3 (SX1262 or SX1276/78) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1276/78 **or** SX1262 — detected at boot | 0.96" SSD1306 (I²C) | microSD, battery ADC | verified (SX1276), SX1262 expected |
| `esp32s3-qspi` | Generic ESP32-S3 DevKitC-1 + SX1262 module | ESP32-S3: 8 MB flash, quad PSRAM | SX1262 | optional SSD1306 | — | builds; wire per flags |
| `tbeam` | LilyGO T-Beam v1.1/v1.2 (SX1276 or SX1262) | ESP32: 4 MB flash, 4 MB PSRAM | SX1276 (v1.1) **or** SX1262 (v1.2) — detected at boot | 0.96" SSD1306 (I²C) | 18650 holder, AXP192/AXP2101 PMU, u-blox GPS, PPP over the CH9102 bridge; **no SD slot** | verified on hardware — see the T-Beam notes below; PPP built, not yet run on this board |
| `t3s3-sx1280` | LilyGO T3-S3 with SX1280 (2.4 GHz) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1280 | 0.96" SSD1306 | microSD, battery ADC | verified on hardware |
| `t3s3-sx1280-pa` | LilyGO T3-S3 with SX1280 + PA (2.4 GHz) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1280 + PA | 0.96" SSD1306 | microSD, battery ADC | **builds only — never run on hardware**, see below |
| `heltec-ws` | Heltec Wireless Stick V2/V2.1 | ESP32: 8 MB flash | SX1276 | 0.49" 64x32 SSD1306 on Vext | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware; PPP built, not yet run on this board |
| `heltec-wb` | Heltec Wireless Bridge | ESP32-D0WDQ6: 8 MB flash, 8 MB PSRAM | SX1276 | — (headless) | front LEDs for Wi-Fi and LoRa (the BLE one stays dark), aluminium shell, two SMA sockets, internal 2-pin battery connector, PPP over the CP2102 bridge; no SD, no GNSS | verified on hardware; PPP built, not yet run on this board |
| `heltec-v3` | Heltec WiFi LoRa 32 V3 | ESP32-S3: 8 MB flash, no PSRAM | SX1262 (TCXO, DIO2 drives the RF switch) | 0.96" SSD1306 on the switched Vext rail | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware; PPP built, not yet run on this board |
| `heltec-wp` | Heltec Wireless Paper | ESP32-S3: 8 MB flash, no PSRAM | SX1262 (TCXO, DIO2 drives the RF switch) | 2.13" e-ink (250x122, E0213A367), driven | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware (console, Wi-Fi, transport, SX1262 self-test, e-paper panel); PPP built, not yet run on this board |
| `heltec-v4` | Heltec V4 (TFT) | ESP32-S3: 16 MB flash, 2 MB PSRAM in package | SX1262 (TCXO, DIO2 drives the RF switch) behind a GC1109 or KCT8103L front end | 2.4" 240x320 ST7789 with a CHSC6X touch layer | two buttons, piezo sounder, battery ADC, GNSS on the expansion header (no SD) | builds; NOT verified on hardware — no pin below has been run |
<!-- boards.json:end -->

### The Wireless Stick's memory

`heltec-ws` is the tightest board here and the only one whose defaults differ
because of it. A classic ESP32 splits its internal memory, and much of what the
heap reports is 32-bit-only IRAM that cannot hold a buffer or a stack — so the
number that matters is the byte-addressable half, which `STATUS` reports as
`dram_free` and `Diag::cost()` bills per subsystem at boot. The Wireless Bridge
is the same silicon with 8 MB of PSRAM and the T-Beam's board file brings 4 MB,
so neither is in this class; `BOARD_DRAM_TIGHT` in `Config.h` selects it by
what the silicon has rather than by name.

Measured on the board: about 213 KB of it exists, and starting a node spent
200 KB, leaving 7 604 B with a low-water mark of 816 bytes and a largest free
block of 4 084 B. That is what took it down with `std::bad_alloc` more than
once. Two defaults differ here:

| | Default | This board | Why |
|---|---|---|---|
| mDNS | on | off | `6 368 B`, and nothing depends on it. A setting (`maintenance.mdns`) |
| `RNS_MAX_CLIENTS` | 4 | 2 | past the cap a client is **refused**, which is a trade an operator can see. Compile-time |

That takes it to 12 220 B free, the low-water mark to 1 264 B and the largest
free block to 4 596 B. Only mDNS is a setting; the client cap is a `#define`
and changing it needs a rebuild.

`AUTOIF_MAX_PEERS` is deliberately **not** reduced here, though it looks like
the obvious next one. Past the cap `AutoInterface::touchPeer()` evicts the
oldest peer rather than refusing the newest, and each eviction deregisters an
RNS interface and drops its stored paths — so a smaller table on a busy bench
buys memory with continuous path flapping, which is a worse failure than the
one being fixed and a much harder one to recognise.

The memory it would have bought is real, though, and points at where the next
work is: `RNS_MAX_INTERFACES` is `1 + RNS_MAX_CLIENTS + AUTOIF_MAX_PEERS`, and
the snapshot buffers are sized from it — so a node pays for peers it may never
have rather than for the ones it has. Sizing those to what is actually
registered would give the board the same memory without touching the cap.

Both Heltec boards use `partitions/huge_app_8mb.csv` rather than the stock
table, which maps only the first 4 MB of an 8 MB part. Neither has an SD slot,
so the filesystem is the only home the Reticulum store has, and the spare flash
goes to it: 4900 KB instead of 896 KB.

The Heltec V3 reaches the host through a CP2102 bridge rather than the S3's own
USB, so it appears as `/dev/ttyUSB*` and not as an Espressif JTAG device. Every
CP2102 reports the serial number `0001`, so with more than one attached
`/dev/serial/by-id/` names only one of them and the rest have to be found by
path.

### Wireless Bridge: the Stick without the panel
`heltec-wb` is the Wireless Stick's ESP32 and SX1276 — the same pins, the
same CP2102, the same 8 MB part and partition table — in an aluminium box with
two SMA sockets and no display, plus 8 MB of PSRAM, which the build enables
with the cache workaround the revision-1 silicon needs and `main.cpp` then
hands the larger allocations to. Nothing that cannot run on it is compiled
for it: `HAS_DISPLAY` is 0, so the panel driver, the page layouts and the QR
renderer stay out of the image. Its three front LEDs are what it shows
instead — see *LEDs* below.

### LEDs
A board names the LEDs it has in its board header (`PIN_STATUS_LED`,
`PIN_WIFI_LED`, `PIN_LORA_LED`; the Wireless Bridge has the Wi-Fi and LoRa
lamps, the Wireless Stick and the Heltec V3 a status LED). Dark is the
normal state — the service is up and idle. A flicker is traffic. A lamp that
stays lit says that service is meant to be up and is not, so every LED
lights at boot and goes out as its service comes up, and a steady lamp
later is the one worth a look.

| LED | lit | flicker | blink |
|---|---|---|---|
| status | the transport is down | — | a restart is on its way |
| wifi | Wi-Fi is on but no link is ready | a packet from a TCP client | — |
| lora | the radio is offline | a packet sent or received | — |

The Wireless Bridge's third lamp is labelled BLE; nothing in this firmware
speaks Bluetooth, so it is claimed and kept dark rather than lit for
something else. In the Battery power profile every LED stays dark.

### The PA variant ships untested
`t3s3-sx1280-pa` is built and published like every other board, but nothing in
it has been measured. Its RF switch pins and its 20 dBm ceiling were taken from
the reference firmware rather than from hardware, and a wrong RF switch means
transmitting into a disabled path. Treat its output power and range as
unverified until someone runs it.

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

## Heltec V4 (TFT)

The `heltec-v4` environment is written from the board's published pin map and has **not
yet run on hardware** — it is the only board in the table in that state, and this section
is the checklist for the first session on the bench.

What is different about this board, in the order it can bite:

**The radio sits behind an amplified front end.** An SX1262 wired straight to its antenna
works the moment SPI does; this one is deaf and mute until the front end's rail is up and
its mode pins are driven (`src/radio/LoRaFem.h`). The firmware powers it before the radio
is probed and points it per direction around every frame. Two parts exist across board
revisions — GC1109 and KCT8103L — wired to mostly the same pins; which is fitted is read
off the shared enable net at boot and the log says which was found. The boot self-test
transmission is the proof: tens of milliseconds to TxDone through a working front end, the
full timeout through a dead one.

**The configured TX power is the chip's drive, not the antenna's.** House convention, as
on the SX1280+PA board — but here the amplifier adds 7–13 dB depending on drive, so the
default 7 dBm leaves the antenna at roughly 18 dBm. That is legal in the 869.4–869.65 MHz
sub-band the default channel sits in and **over the limit in most of the rest of EU868**:
changing frequency on this board is a power decision too.

**The panel is colour, driven monochrome at half resolution.** Pages draw on a 120x160
canvas and the panel shows each pixel as a 2x2 block (`src/ui/TftPanel.h`); at the glass's
dot pitch that lands the text at the e-paper's size. The panel is write-only — no probe
can tell whether it is there, so a wrong pin map shows as a dark panel with a healthy log.

**Verification checklist, first bench session:**

1. Boot log: which front end was detected, and the self-test's TxDone time.
2. A frame heard by another node — the front end's TX path proven on air.
3. A frame *received* from a distant node — the LNA path, which the self-test cannot prove.
4. The panel lights and shows the status page; touch is polled (`chsc6x` at 0x2E on its
   own I2C pair) and turns pages.
5. Battery: a plausible voltage with a cell attached, and `0.0` without one — the divider
   is switched (GPIO 37) and deep (÷5.1), both firsts here.
6. GNSS: sentences counted on the GPS page with the expansion kit fitted.
7. An OTA update staged on LittleFS and installed — this is the first board that stages
   without an SD card, and the first flashed A/B from day one.

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
| Battery ADC | 1 (100k/100k divider) |

On the SX1280 variants the busy and interrupt lines move: **BUSY 36, DIO1 9**.
Those were established with a GPIO scan during transmission, because the boot
log is byte-identical whether they are right or wrong — the radio initialises
either way and simply never reports a completed transmission.

There is no charge-status line on any T3-S3 variant. The divider measures the
cell; whether it is charging is a question the board cannot answer, and the API
reports null rather than claiming it is idle. Only boards with a PMU know.
| microSD MOSI / MISO / SCK / CS | 11 / 2 / 14 / 13 (HSPI, separate from the radio bus) |

The SX1262 probe waits on BUSY (GPIO 34 = DIO2 on SX127x boards) and takes
~27 s to fail, so the SX127x is probed first (~100 ms to fail on SX1262).

## microSD card
Optional; hot-plug polled every 3 s. The card is mounted as one FAT volume at
`/sd`. Status values: `mounted`, `partial` (the FAT volume covers less than
half the card — e.g. a Raspberry Pi image with a small boot partition),
`unformatted` (no filesystem the node recognises), `formatting`, `error`,
`absent`. The settings page can format the whole card to a single FAT32
volume (admin, explicit confirmation — erases everything), and refuses while
the store is on the card or a move is in progress.

The card holds `/retimesh/events.log` (announces, boots; rotated at 1 MB,
downloadable from the portal), `/retimesh/store.json` naming the node that owns
the store, and `/rns` when the store has been moved onto it. Use the card for
the store with **Use this card** on the settings page and take it back with
**Eject**; both copy the data across and restart the node into its new home. See
[Architecture](architecture.md#the-store-has-one-home).

An empty slot reports `absent` with a capacity of zero. If you ever see a
capacity that looks invented, that is the bug fixed in v0.0.8: the presence
check used to report a card whenever a driver slot was free and read its size
from a field nothing had written.

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

## Host connectivity and flashing
What each board puts on its USB connector, which bootloader-entry methods it
offers and which IP local links it could carry are in the capability matrix in
[local-link.md](local-link.md#board-capability-matrix). In short: the four
native-USB S3 boards and the Heltec V3 can restart into their ROM downloader on
request (`BOOTLOADER CONFIRM` on the console, `POST /api/system/bootloader`);
the classic-ESP32 boards rely on the bridge's DTR/RTS reset, which esptool
performs; and BOOT + RST recovers any of them.

## Adding a board
1. `src/boards/<name>.h`: the pin map and the capability flags (`HAS_SD`,
   `HAS_PMU`, `HAS_GPS`, `HAS_DISPLAY`, `HAS_BATTERY_ADC`, `BOARD_NAME`).
   Everything in `Config.h` is `#ifndef`-guarded, so the board header wins and
   anything it omits falls back to a sensible default. Host connectivity is
   **not** declared here — it comes from `boards.json` (next step).
2. `src/Config.h`: one line in the board-selection block mapping `-DBOARD_<X>`
   to the header.
3. `platformio.ini`: a new `[env:<name>]` (board, partitions, `-DBOARD_<X>`,
   and `build_unflags` for anything the base env sets that the board lacks —
   PSRAM and native-USB CDC are the usual ones).
4. `boards.json`: name, chip family, notes, and the `local_link` block — what
   is on the USB connector (`usb.native` or `usb.bridge`), whether the bridge's
   DTR/RTS reset the chip, whether the UART may carry PPP, which UART it is,
   the speeds it may run at and the highest one actually tried
   (`qualification`, `tested_max_baud`). `tools/board_caps.py` turns it into `BOARD_*` flags at build time
   and `tools/check_boards.py` (CI) refuses a board without one, one whose
   connector facts contradict its chip, or one that
   contradicts the framework's USB flags in `platformio.ini`. Drives CI,
   release packaging, the web flasher and the CLI.
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
assumption. `GET /api/settings` reports them under `radio.caps`: tuning range, the
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
