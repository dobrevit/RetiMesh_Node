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
| `heltec-v4` | Heltec V4 (TFT) | ESP32-S3: 16 MB flash, 2 MB PSRAM in package | SX1262 (TCXO, DIO2 drives the RF switch) behind a GC1109 or KCT8103L front end | 2.4" 240x320 ST7789 with a CHSC6X touch layer | two buttons + case power button (charger /QON), DA217 accelerometer, BQ25896-ready charging state, battery ADC, GNSS on the expansion header; expansion slots for sounder/sensors (no SD) | verified on hardware: radio through the KCT8103L front end (TX and RX on air), GNSS fix + clock, panel, touch, both buttons; sounder silent — possibly not fitted |
| `t-deck` | LilyGO T-Deck | ESP32-S3: 16 MB flash, 8 MB octal PSRAM in package | SX1262 (TCXO at 1.8 V, DIO2 drives the RF switch), no amplifier | 2.8" 320x240 ST7789 with a GT911 touch layer | physical keyboard on its own microcontroller (I2C), trackball with a click on the BOOT pin, microSD, battery ADC, speaker and microphone (not driven); GNSS on the Plus only | verified on hardware: SX1262 on air (receiving announces from another node), Reticulum transport, panel with correct colours and text, its stepped backlight, touch landing where the finger is, keyboard, trackball, microSD on the shared bus, GNSS fix and clock, Wi-Fi AP, portal and console. This is a Plus — a plain T-Deck has neither the receiver nor the touch layer. The battery divider reads the system rail on USB and so reports no cell; unverified on battery |
| `thinknode-m9` | Elecrow ThinkNode M9 | ESP32-S3: 16 MB flash, 8 MB octal PSRAM in package | Semtech LR1110 (TCXO at 3.3 V, the chip drives its own antenna switch from DIO5/DIO6) | 2.4" 320x240 ST7789, no touch layer | full keyboard on its own microcontroller (I2C, register-addressed) with six shortcut keys wired to the shell's screens, ATGM336H GNSS, microSD, sounder, PCF8563 RTC, QMI8658 IMU, QMC6309 magnetometer, 2300 mAh cell behind an LGS4056 charger; PPP over the CH340 bridge | verified on hardware: LR1110 online and on air (boot self-test TxDone in 7 ms through the chip's own antenna switch, and a packet received), panel, keyboard, microSD on the shared bus (15.6 GB SDHC, Reticulum store on the card), GNSS talking (308 sentences, no fix indoors), Wi-Fi AP, mDNS, portal and Reticulum transport. Two caveats: the transceiver's own firmware is 0x0303, old enough that RadioLib has to skip a command for it (see the env's RadioLib pin) and old enough to predate Semtech's security fixes; and the panel's rotation and the battery divider are taken from the sources rather than measured. The PCF8563 clock is driven: it was already holding correct UTC before this firmware ever wrote it, seeds the system clock at boot, and is re-seeded from the receiver — so this board's time is right indoors with no fix, where every other board here counts from 1970 until the sky clears |
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

The `heltec-v4` environment was written from the board's published pin map and then
proven on the bench (2026-09-02): radio both directions on air through the front end,
GNSS fix and clock, panel, touch and both buttons — the table row above carries the
status. The sounder alone stayed silent and may not be fitted on every build of the
board. What follows is what that first session had to know; a new unit re-checks the
same list.

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

**Flashing, once this firmware is on it:** the OTG composite CDC does not wire the
DTR/RTS bootloader dance, so esptool alone cannot enter the downloader — the upload
hook's hand-off (the default) asks the running node over the console and works every
time. `RETIMESH_NO_AUTO_BOOTLOADER=1` is only for a factory-fresh board that cannot
answer; set it on this board once our firmware runs and the upload fails with
"No serial data received". The by-id name is the RetiMesh identity while the
application runs and the Espressif JTAG identity in the downloader.

**What the bench verified, in the order a new unit should re-check it:**

1. Boot log: which front end was detected, and the self-test's TxDone time.
2. A frame heard by another node — the front end's TX path proven on air.
3. A frame *received* from a distant node — the LNA path, which the self-test cannot prove.
4. The panel lights and shows the status page; touch is polled (CHSC6X at 0x2E on its
   own I2C pair) and speaks the button's grammar — tap turns the page, holding blanks the
   panel. The second case button walks the pages backwards, and its long press deliberately
   does nothing, so a button held by a case or a pocket cannot blank the panel. The sounder
   plays two notes up when the node finishes booting and one high note when a message for
   this node arrives.
5. Battery: a plausible voltage with a cell attached, and `0.0` without one — the divider
   is switched (GPIO 37) and deep (÷5.1), both firsts here.
6. GNSS: sentences counted on the GPS page with the expansion kit fitted.
7. An OTA update staged on LittleFS and installed — this is the first board that stages
   without an SD card, and the first flashed A/B from day one.

## LilyGO T-Deck

The `t-deck` environment. This is the first board here that is a *terminal* rather than a
gateway: it has a keyboard, so the LVGL shell's on-glass keyboard steps aside and the text
field takes the real keys instead.

Its pin map is the best-sourced in this registry. Four independent firmwares — LilyGO's own
reference, Meshtastic, MeshCore and the Zephyr board port — agree on every number that
matters, against the one published source the Heltec V4 had. What follows is the part that
sourcing did *not* settle, in the order it can bite.

**Nothing on the board answers until GPIO 10 is high.** The radio, the card, the panel, the
keyboard and the touch controller all sit behind one load switch. Probe before raising it
and you get an empty I2C bus and a transceiver that does not reply — which reads exactly
like a wiring fault and is not one. It is not a PMU, so `Pmu.h` has nothing to say about it;
it is one GPIO and it has to be the first one. `BoardInit::begin()` raises it at the top of
`setup()`, before the filesystem, before any bus, before the radio.

**The panel, the radio and the card share one SPI bus.** Every other board here gives the
panel a bus of its own precisely so the radio never waits behind a blit. This board does not
offer the choice — all three chip selects hang off one set of wires, and the S3's GPIO matrix
cannot let two peripherals drive one output pin. Sharing is safe rather than merely
tolerated: `SPIClass` objects built on the same bus number resolve to the same underlying bus
struct, whose mutex `beginTransaction` takes and `endTransaction` releases, so the radio task
and the display task serialise in the core rather than by convention; and no driver here
attaches a hardware chip select, so each driver's `digitalWrite` is the only thing moving a
CS line. What it costs is latency — a full-frame blit holds the bus for a few milliseconds
and a packet arriving during one waits — and nothing is dropped, because receive is
interrupt-driven into a task that reads the chip afterwards.

Two things no single driver could do for itself. The first is the *starting* state: each
raises its own select in its own `begin()`, which is correct and too late — whichever runs
first is talking on a bus where the other two selects are still floating, and a floating
select is a device that may decide it is being addressed. `BoardInit::begin()` idles all
three together before any of them exists.

The second is owning the bus. Arduino's `SPIClass::begin()` guards against being called
twice on *itself*, not against another object starting the same peripheral — so the second
driver's `begin()` re-runs the whole bus setup underneath the first, re-registering the
APB-change callback the core then refuses as a duplicate. That log line
(`addApbChangeCallback(): duplicate func=...`) is the only warning, and the boot stops just
after it. So a host is fetched rather than constructed: `SpiBus::get()` hands out one object
per host, started by the first caller, and the panel, the radio and the card all hold a
pointer to it. Sharing the object is what makes sharing the wires safe — the core's per-bus
mutex only excludes drivers that agree on which bus they are on.

**The backlight is not an LED on a PWM pin.** GPIO 42 drives an AW9364 one-wire dimmer whose
brightness is a counter inside the part: holding the line high turns it on at full, and each
further low-high pulse steps it down one of sixteen levels, wrapping at the bottom. Driving
it with a PWM channel — which is what this firmware does on every other colour board, and
what one of the two reference firmwares for this board does — sends twenty thousand step
pulses a second and lands wherever the wrap leaves it. It lights, which is how the mistake
survives; it is not a dimmer. `BACKLIGHT_KIND` picks the part's own protocol here.

**The I2C bus runs at 100 kHz, not 400.** The keyboard's controller brings its slave up at
100 kHz and shares the bus with the touch controller, so the slowest part on the wire sets
the rate for all of it. `I2C_HZ` carries that.

**The keyboard is a latch, not a state.** A microcontroller of its own scans the keys and
hands over one key per read; there is no release event and nothing to debounce. A missed read
is a lost keystroke, which is why the shell polls it as an LVGL keypad device rather than
sampling it with the page timer. The trackball beside it pulses once per detent on four
GPIOs and says nothing between pulses, so its edges are counted in an interrupt — a poll at
the display's rate would see a level rather than the movement. Both arrive as one stream of
key codes (`src/ui/Keypad.h`), because a detent is an arrow key by any sensible reading.

**The trackball's click is the BOOT pin**, which is also this board's only button. That is
why holding the ball down while pressing reset is the documented way into the ROM
downloader — and why `PIN_BUTTON` and the click are the same number.

GNSS is fitted on the T-Deck **Plus** only; on the plain board GPIO 43/44 reach the Grove
connector and nothing else. Both variants are this one env and it assumes the receiver is
there — `HAS_GPS` is 1 — which is the same call this project already made for the Heltec
V4's expansion kit: the bench unit is a Plus, and a board without a receiver loses nothing
but a UART nobody is talking on. A plain T-Deck reports a receiver that never sends a
sentence, which the GPS page states plainly rather than hiding.

### T-Deck pin map
| Function | GPIO |
|---|---|
| Peripheral rail (raise first) | 10 |
| SPI SCK / MISO / MOSI (shared) | 40 / 38 / 41 |
| LoRa CS / RST / BUSY / DIO1 | 9 / 17 / 13 / 45 |
| TFT CS / DC / RST / backlight | 12 / 11 / tied / 42 |
| Touch (GT911 @ 0x5D) SDA / SCL / INT | 18 / 8 / 16 |
| Keyboard (@ 0x55) SDA / SCL | 18 / 8 |
| Trackball up / down / left / right | 3 / 15 / 1 / 2 |
| Button, and the trackball click | 0 (BOOT) |
| microSD CS | 39 |
| Battery ADC (÷2, always connected) | 4 |
| GNSS RX / TX (Plus only) | 44 / 43 |

Reserved on this part and unavailable: 26–32 (SPI flash) and **33–37 (octal PSRAM)** — an
R8 die is octal, which is what `qio_opi` in the env says and what the chip measures as.
19/20 are the native USB pair and carry the console.

## Elecrow ThinkNode M9

The `thinknode-m9` environment, and the first board here whose radio is not a Semtech
SX12xx. Most of the rest of it is ground already covered — a shared SPI bus and a gated
peripheral rail, both of which the T-Deck brought in — so what follows is mostly about the
radio.

**The LR1110 drives its own antenna switch, and does nothing until told how.** Every other
radio here either connects the antenna by itself (a bare SX1262) or has the MCU steer a
switch in front of it (the SX1280+PA, the amplified Heltec V4). This one steers its own,
from its own DIO lines, and it needs a table saying which line means what in each mode.
Until it has one the part answers over SPI, reports a firmware version, accepts a channel
and transmits into a pin that goes nowhere — **online by every measure this firmware has,
and silent**. That is the same failure the amplified V4 has, arriving by a different route,
and it is why this radio is declared by the board (`RF_MODEM_LR1110`) rather than probed
for: the table is board wiring, not chip behaviour. It lives in the board header as
`LR11X0_RF_SWITCH_TABLE`, and `probeLR1110()` writes it to the chip immediately after
`begin()`. The boot self-test transmission is the proof it took.

This board wires DIO5 and DIO6 only. Note the high-frequency transmit row is identical to
standby: the 2.4 GHz path the LR1110 could otherwise offer is not routed here, which is why
`RadioCaps::kLR1110` describes a sub-GHz radio rather than a dual-band one. Claiming the
range would let the validator accept a channel the board cannot radiate.

**Its bandwidth list is four steps, not ten.** Below 1 GHz the LR11x0 offers 62.5, 125, 250
and 500 kHz. The SX126x offers ten values including 41.7 and 20.8, and a node reflashed from
one of those boards still holds the old figure in NVS. Left alone, `begin()` fails on the
bandwidth and the log blames the wiring, so the channel is corrected once before the probe —
the same treatment the SX1280 boards get, for a narrower reason.

**The peripheral rail is active low here.** GPIO 18 gates the panel and the sensor bus, and
Elecrow's own documentation calls it VDD_PERIPH_EN. The T-Deck's equivalent is active high,
which is why the level is part of the board description rather than assumed by the code that
raises it.

**The chip's own USB pins are spent on other things.** GPIO 19 and 20 are the ESP32-S3's
D−/D+, and this board uses 19 for the panel's tearing signal and 20 for the keyboard's I2C
data. Left enabled the USB peripheral drives those pins alongside the keyboard bus, which
then reads as stuck — a wiring fault that is not one. `BoardInit` releases the pad before
anything else, which costs nothing because the console here is a CH340 bridge on UART0.

**The keyboard is register-addressed**, unlike the T-Deck's, which answers a bare read. Here
the key register is written first and read back with a repeated start; a bare read would
return whatever the controller's pointer was left sitting on. It also has an I2C bus to
itself, away from the RTC and the sensors. `KEYPAD_KIND` picks the protocol and the driver
translates the controller's own arrow codes so that nothing above it has to know which board
it is running on.

**There is no touch layer at all** — the first colour board here without one. The shell is
therefore driven entirely from the keys, which is what the keypad group does: every
focusable widget the shell builds joins it automatically, so arrows move the focus and Enter
activates. On a board with touch that is a convenience; here it is the difference between a
usable node and an ornament.

Two smaller ones: the backlight lights when its pin is **low**, and the battery divider is on
GPIO 13, which is **ADC2** — the converter the radio and Wi-Fi stack contend for, so a
reading can fail while they are busy. That is a missed sample rather than a wrong one.

### ThinkNode M9 pin map
| Function | GPIO |
|---|---|
| Peripheral rail (VDD_PERIPH_EN, **active low**) | 18 |
| SPI SCK / MISO / MOSI (shared) | 40 / 38 / 47 |
| LR1110 NSS / RST / BUSY / IRQ (DIO9) | 39 / 45 / 41 / 42 |
| LR1110 antenna switch | the chip's own DIO5 / DIO6 |
| TFT CS / DC / RST / backlight (**active low**) | 16 / 15 / 14 / 17 |
| Keyboard (@ 0x6C) SDA / SCL / INT / backlight | 20 / 21 / 12 / 46 |
| Sensor I2C SDA / SCL (RTC, IMU, magnetometer) | 7 / 6 |
| GNSS RX / TX / EN / RST / PPS / standby | 2 / 3 / 11 / 5 / 4 / 10 |
| microSD CS | 48 |
| Battery ADC (÷2, on ADC2) | 13 |
| Sounder | 9 |
| Console (CH340 bridge, UART0) | 43 / 44 |

Reserved and unavailable: 26–32 (SPI flash) and **33–37 (octal PSRAM)**. GPIO 45 is the
VDD_SPI strapping pin as well as the radio's reset — never pull it up. GPIO 39–42 are the
JTAG pins and all four are taken by the radio, so there is no on-chip debug on this board.

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
5. Nothing in the workflows: CI, the release matrix and the HIL run all read
   `boards.json` and build whatever is in it. (A bench runner wants a
   `HIL_<ENV>_PORT` repository variable to exercise the new board, but the
   build matrices need no edit — they used to, and a board once reached the
   registry without reaching them.) Regenerate the table above with
   `python tools/board_docs.py`, which CI checks, and run
   `python tools/check_boards.py`.
6. If the display or radio differ: `Display.*` / `LoRaRadio.*` (probe order,
   TCXO, RF switch). Keep board specifics behind the capability flags. A driver
   that needs SPI asks `SpiBus::get()` for the host rather than constructing an
   `SPIClass` — on a board where two devices share wires, two objects on one
   host re-initialise the peripheral under each other and the boot stops.
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
