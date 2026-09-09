# Hardware

## Supported boards
<!-- boards.json:begin -->
<!-- Rendered by tools/board_docs.py from boards.json. Edit the registry, not this table. -->
| Env | Board | MCU | Radio | Display | Extras | Status |
|---|---|---|---|---|---|---|
| `t3s3` | LilyGO T3-S3 v1.2/v1.3 (SX1262 or SX1276/78) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1276/78 **or** SX1262 — detected at boot | 0.96" SSD1306 (I²C) | microSD, battery ADC | verified (SX1276), SX1262 expected |
| `esp32s3-qspi` | Generic ESP32-S3 DevKitC-1 + SX1262 module | ESP32-S3: 8 MB flash, quad PSRAM | SX1262 | optional SSD1306 | — | builds; wire per flags |
| `tbeam` | LilyGO T-Beam v1.1/v1.2 (SX1276 or SX1262) | ESP32: 4 MB flash, 4 MB PSRAM | SX1276 (v1.1) **or** SX1262 (v1.2) — detected at boot | 0.96" SSD1306 (I²C) | 18650 holder, AXP192/AXP2101 PMU, u-blox GPS, PPP over the CH9102 bridge; **no SD slot** | verified on hardware — see the T-Beam notes below; PPP built, not yet run on this board |
| `tbeam-supreme` | LilyGO T-Beam Supreme | ESP32-S3FN8: 8 MB flash, 8 MB quad PSRAM | SX1262 in a socketed module (TCXO at 1.8 V, DIO2 drives the RF switch) | 1.3" 128x64 SH1106 (I²C) — not the SSD1306 the other OLED boards carry | 18650 holder, AXP2101 PMU owning six rails, u-blox MAX-M10S **or** Quectel L76K GNSS, microSD, PCF8563 RTC, Qwiic socket; BME280 driven (temperature, humidity, pressure); QMI8658 IMU on SPI — driven, and faulty on the unit tested here: LilyGO's own example finds every other part on the board and cannot find this one either; QMC6310N magnetometer driven at 0x3c (heading, field strength and hard-iron calibration; uncorrected for tilt on this unit, whose accelerometer is faulty) | verified on hardware 2026-09-08: SX1262 on air both ways (rx 24, tx 7 at 869.525, rssi -34, snr 12.5), AXP2101 found on its own I2C host and every rail it feeds alive — radio, panel, clock and card slot, microSD mounted (8 GB SDHC, Reticulum store moved onto it), PCF8563 holding time to a second, GNSS talking (932 sentences, no fix indoors), 8 MB of PSRAM free, the SH1106 panel rendering its pages and stepping through them on the button, and the BME280 reading 27.7 C, 60 % and 1012.7 hPa. The panel is at **0x3D**, not the 0x3C that both its address probe and the RNode firmware point at: 0x3c is the QMC6310 magnetometer, which took an entire SH1106 initialisation without complaint because a register file accepts any write, while the glass kept the previous firmware's picture and every status field reported the display fine. The three parts on that bus are now each identified by asking them — 0x3c magnetometer, 0x3d panel, 0x77 BME280 — The 6-axis part is fitted and does not answer: its select works and something drives MISO when it is asserted, but every register reads zero, no write lands, the vendor's reset never completes, and none of it changes across all four SPI modes, two clock rates, a hand-clocked control, or every rail the AXP2101 has. LilyGO's own QMI8658 example, built from their repository with their board definition and rail init, finds the magnetometer, panel, BME280, card and GNSS and then reports "Failed to find QMI8658 - check your wiring!" — so it is a hardware fault on this unit rather than anything a firmware change reaches. Their magnetometer example does read the QMC6310N here, which is what made driving it a port rather than a research task — it is now driven by this firmware, whose register values and scale are the ones that example was running. Their GNSS probe identifies this unit's receiver as the u-blox MAX-M10S. USB composite with usb0 ready, Wi-Fi AP and transport online. One thing the bench has not settled: whether a rail-cut GNSS nap costs a cold start here |
| `t3s3-sx1280` | LilyGO T3-S3 with SX1280 (2.4 GHz) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1280 | 0.96" SSD1306 | microSD, battery ADC | verified on hardware |
| `t3s3-sx1280-pa` | LilyGO T3-S3 with SX1280 + PA (2.4 GHz) | ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM | SX1280 + PA | 0.96" SSD1306 | microSD, battery ADC | **builds only — never run on hardware**, see below |
| `heltec-ws` | Heltec Wireless Stick V2/V2.1 | ESP32: 8 MB flash | SX1276 | 0.49" 64x32 SSD1306 on Vext | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware; PPP built, not yet run on this board |
| `heltec-wb` | Heltec Wireless Bridge | ESP32-D0WDQ6: 8 MB flash, 8 MB PSRAM | SX1276 | — (headless) | front LEDs for Wi-Fi and LoRa (the BLE one stays dark), aluminium shell, two SMA sockets, internal 2-pin battery connector, PPP over the CP2102 bridge; no SD, no GNSS | verified on hardware; PPP built, not yet run on this board |
| `heltec-v3` | Heltec WiFi LoRa 32 V3 | ESP32-S3: 8 MB flash, no PSRAM | SX1262 (TCXO, DIO2 drives the RF switch) | 0.96" SSD1306 on the switched Vext rail | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware; PPP built, not yet run on this board |
| `heltec-wp` | Heltec Wireless Paper | ESP32-S3: 8 MB flash, no PSRAM | SX1262 (TCXO, DIO2 drives the RF switch) | 2.13" e-ink (250x122, E0213A367), driven | PPP over the CP2102 bridge (no SD, no GNSS) | verified on hardware (console, Wi-Fi, transport, SX1262 self-test, e-paper panel); PPP built, not yet run on this board |
| `heltec-v4` | Heltec V4 (TFT) | ESP32-S3: 16 MB flash, 2 MB PSRAM in package | SX1262 (TCXO, DIO2 drives the RF switch) behind a GC1109 or KCT8103L front end | 2.4" 240x320 ST7789 with a CHSC6X touch layer | two buttons + case power button (charger /QON), DA217 accelerometer, BQ25896-ready charging state, battery ADC, GNSS on the expansion header; expansion slots for sounder/sensors (no SD) | verified on hardware: radio through the KCT8103L front end (TX and RX on air), GNSS fix + clock, panel, touch, both buttons; sounder silent — possibly not fitted. The DA217 accelerometer is suspended while the screen is dark and comes back with it — a dark panel needs no orienting — and `STATUS` reports it as `imu=asleep`; the suspend register value is the kernel da280 driver's and has not yet been exercised on this board, so that line is what confirms it took |
| `t-deck` | LilyGO T-Deck | ESP32-S3: 16 MB flash, 8 MB octal PSRAM in package | SX1262 (TCXO at 1.8 V, DIO2 drives the RF switch), no amplifier | 2.8" 320x240 ST7789 with a GT911 touch layer | physical keyboard on its own microcontroller (I2C), trackball with a click on the BOOT pin, microSD, battery ADC, speaker on a MAX98357A over I2S (driven: the boot and message chimes), microphone on an ES7210 (not driven — it costs GPIO 0, which is the trackball's click); GNSS on the Plus only | verified on hardware: SX1262 on air (receiving announces from another node), Reticulum transport, panel with correct colours and text, its stepped backlight, touch landing where the finger is, keyboard, trackball, microSD on the shared bus, GNSS fix and clock, Wi-Fi AP, portal and console, and the speaker — the boot chime was heard on the bench. The speaker costs about 5.4 KB of internal RAM for its task and I2S DMA, on a board that has little to spare, and its task now carries the tightest stack of any on the board. This is a Plus — a plain T-Deck has neither the receiver nor the touch layer. The battery divider reads the system rail on USB and so reports no cell; unverified on battery |
| `thinknode-m9` | Elecrow ThinkNode M9 | ESP32-S3: 16 MB flash, 8 MB octal PSRAM in package | Semtech LR1110 (TCXO at 3.3 V, the chip drives its own antenna switch from DIO5/DIO6) | 2.4" 320x240 ST7789, no touch layer | full keyboard on its own microcontroller (I2C, register-addressed) with six shortcut keys wired to the shell's screens, ATGM336H GNSS, microSD, sounder, PCF8563 RTC, QMI8658 IMU, QMC6309 magnetometer, 2300 mAh cell behind an LGS4056 charger; PPP over the CH340 bridge | verified on hardware: LR1110 online and on air (boot self-test TxDone in 7 ms through the chip's own antenna switch, and a packet received), panel, keyboard, microSD on the shared bus (15.6 GB SDHC, Reticulum store on the card), GNSS talking (308 sentences, no fix indoors), Wi-Fi AP, mDNS, portal and Reticulum transport. Two caveats: the transceiver's own firmware is 0x0303, old enough that RadioLib has to skip a command for it (see the env's RadioLib pin) and old enough to predate Semtech's security fixes; and the panel's rotation and the battery divider are taken from the sources rather than measured. The PCF8563 clock is driven: it was already holding correct UTC before this firmware ever wrote it, seeds the system clock at boot, and is re-seeded from the receiver — so this board's time is right indoors with no fix, where every other board here counts from 1970 until the sky clears. The QMI8658 accelerometer and QMC6309 magnetometer are driven — both identified by register dump before a driver was written (WHO_AM_I 0x05, chip id 0x90) — and report a tilt-compensated heading on the console. **That heading changed direction** when the T-Beam Supreme's magnetometer was driven: the tilt correction had gravity's sign convention wrong and returned a bearing reflected about north for a board lying flat — steady, turning with the board, and mirrored. It is fixed and pinned by test_mag_heading, and this board's heading wants re-checking against a known bearing. The heading's hard-iron offsets need the board turned around once before they mean anything, which STATUS reports as cal=; the board's own field is about 385 uT, several times the Earth's and almost all of it perpendicular to the panel, so that correction is not optional here — measured to be the board's rather than the bench's by moving it off a laptop it was resting on, which changed the reading by under a microtesla. The offsets are kept in NVS across restarts for the same reason. Both parts are suspended while the screen is dark and come back with it — so `STATUS` on a node whose display has timed out reports `compass=asleep` rather than a bearing, and any key or button press brings it back within a poll. That is this board's own setting (COMPASS_FOLLOWS_SCREEN 1) rather than a rule about consumers: no page here shows a bearing either, and the reason the M9 sleeps its parts is that it is a handheld on a cell whose standby current with a magnetometer converting at 200 Hz has never been measured. A caller polling /api/status on a dark M9 is told asleep, and loading a web page does not wake the glass |
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

## LilyGO T-Beam Supreme

The `tbeam-supreme` environment. It shares a name with the T-Beam and almost nothing else:
an ESP32-S3 with native USB where the classic board is a bridged ESP32, 8 MB of flash and
8 MB of quad PSRAM where that one has 4 MB and no PSRAM it admits to, two I2C buses, two
SPI buses, a card slot, a clock, and an AXP2101 that owns six rails rather than three. What
does carry over is the shape of the problem: **nothing on this board answers until the PMU
has been told to power it**, so `Pmu::begin()` runs before the radio is probed, and an
unpowered rail reads on the bench exactly like a part that is not fitted.

**Brought up on hardware 2026-09-08, and the pin map held.** It was written from two
sources that agree on every pin — LilyGO's own hardware notes for the board, and the RNode
firmware's block for it (`BOARD_TBEAM_S_V1`), which runs on this hardware — with the rail
map from LilyGO's reference code. What the bench then found is at the end of this section;
the short version is that the radio, the panel, the card, the clock and the receiver all
came up on the first flash, and the five single-sourced pins were right.

**The rail map is the part with no room for inference.** This board's AXP2101 is wired
differently from the T-Beam's, and the T-Beam's map applied here powers a sensor rail and
leaves the radio dark:

| Rail | Feeds | State at boot |
|---|---|---|
| ALDO1 | the panel, the 6-axis part, the magnetometer, the BME280 | on |
| ALDO2 | the sensor bus and the PCF8563 clock | on |
| ALDO3 | the transceiver module | on |
| ALDO4 | the GNSS receiver | **off** until the receiver is switched on |
| BLDO1 | the microSD slot | on |
| DCDC3 | the module socket | on |
| DCDC1 | the system rail the ESP32 itself runs from | left alone |

So which regulator feeds what is a board fact now, not a T-Beam fact: `PMU_RAIL_*` in the
board header, defaulted in `Config.h` to what the T-Beam has always used. DCDC1 is the one
to read twice — on the T-Beam it is the display's rail and gets switched on; here switching
it is switching the board off.

**The panel is an SH1106, not the SSD1306 every other OLED board here carries.** Same 128x64
geometry and the same GFX calls, and a controller with 132 columns of RAM showing the middle
128 and no horizontal addressing mode. Driven by the SSD1306's code it comes up two columns
out of place, wrapping what falls off the end, with an initialisation sequence it only
partly understands. Nothing can be probed for: an acknowledgement at whatever address the
board names says a part is there and nothing about which controller it is. So the board
declares both — `OLED_CONTROLLER` for the part and `OLED_ADDR` for where it answers, which
on this board is **0x3d** and not the address its siblings use. `OledPanel` carries a driver
type rather than a single driver; if a unit turns out to have an SSD1306 fitted instead,
that one macro is the whole change.

**The receiver is a u-blox MAX-M10S or a Quectel L76K, depending on the unit.** Both come up
at 9600 baud speaking NMEA and nothing on the wire says which is there, so the build speaks
NMEA to both (`GPS_UBX 0`) and naps the receiver by cutting ALDO4 — a harder off than any
message, and one that works either way. The L76K's wake line on GPIO 7 is deliberately
*not* declared as the standby pin: naming it would make the nap a pin write that does
nothing on half the units sold instead of a rail cut that works on all of them. A unit
confirmed to carry the MAX-M10S can set `GPS_UBX 1` and gain the binary position and time
frames.

**The BME280 and the accelerometer are both driven, and only one of them answers.** Both
were on the fitted-but-not-driven list when this board landed; the second change to it wrote
both drivers, and the accelerometer — fitted, selected, and silent — is its own section
below.

The QMI8658 is on SPI here — chip select GPIO 34, on the card's bus — where `src/sys/Imu.cpp`
had only ever spoken I2C. That is a transport and not a different part: the registers, the
chip id and the configuration are the M9's, so only the three calls at the top of that file
change (`IMU_TRANSPORT`). Two consequences are worth knowing. `BoardInit` idles **both**
selects on those wires before either driver exists, and does not care which `begin()` runs
first: a floating select is a device that may answer, so an ordering argument is a thing to
get wrong rather than a thing to rely on — and this one was got wrong once, written down as
the card going first when `main.cpp` has the accelerometer at line 267 and the card at 277.
And this is the first board whose accelerometer does **not** follow the screen: nothing
here reads it for a heading or for panel rotation, so its only readers are the console and
the status API, which are asked with the glass dark as often as not. That rule lives in
`PeripheralPolicy.h`, which had said since it was written that its two questions would part
company one day; this is the day — and it is derived rather than declared:
`IMU_FOLLOWS_SCREEN` is `DISPLAY_AUTO_ROTATE || (HAS_COMPASS && COMPASS_FOLLOWS_SCREEN)`.
Both halves are 0 here, and the second half is why: the magnetometer below *is* driven, and
it does not follow the screen either, so it does not hold the accelerometer up. Turn
`COMPASS_FOLLOWS_SCREEN` on and this follows it, which is the point of deriving it.

The BME280 at 0x77 reports temperature, humidity and pressure on the console, `/api/status`,
and a weather page of its own in the display cycle. One forced conversion every thirty
seconds, the part asleep in between, and the compensation arithmetic — eleven factory
coefficients and a polynomial per reading — in `src/sys/Bme280Math.h`, where it is unit
tested against the datasheet's decode rules and clamps. It is read from the main loop and
nowhere else on purpose: the panel is on the same bus, and `I2cReg` drains a read outside
the bus lock, so one reader is what makes that bus safe.

**The census on the panel's bus, which took two bring-ups to get right.** Three devices
answer there and each one has now been identified by asking it:

| Address | What it is | How it was told |
|---|---|---|
| 0x3c | **QMC6310N magnetometer** | chip id 0x80 in register 0, behind a real register file — and LilyGO's own scanner names it QMC6310N at this address |
| 0x3d | the SH1106 panel | the same byte from every register, and bit 6 of it tracks the display being blanked |
| 0x77 | the BME280 | it answers with readings |

That table is the correction to two earlier claims. The magnetometer was declared *not
fitted* on the strength of a scan that found nothing at 0x0d, 0x1c, 0x2c or 0x7c — the
addresses the M9's QMC6309 uses — when the part was answering at 0x3c all along, which is
where a QMC6310N sits. And the "second address that answered" on this bus, blamed on a
phantom when the panel turned out to be at 0x3d, was never a phantom: it was this
magnetometer, which accepted an entire SH1106 initialisation and a kilobyte of framebuffer
per refresh without complaint, because a register file will take any write you send it.

**And it is driven — which turned up a defect in the heading itself.** Driving a second part
meant lifting the bearing arithmetic out of the driver into `src/sys/MagHeading.h`, where a
host can check it, and the first test written against the stated convention failed: the tilt
compensation had gravity's sign convention backwards. The standard form is derived for an
accelerometer reading +1 g on the axis pointing down, these parts read −1 g lying face up,
and fed that the rotation returned a bearing **reflected about north** — 315° where the same
field read flat gives 45°. Steady, turning with the board, and wrong. It had shipped that way
on the ThinkNode M9 since its compass was written, because nothing compared the corrected and
uncorrected paths in the one case where they must agree. The Supreme never hit it (no working
accelerometer, so it always takes the flat path), the M9 always did, and `test_mag_heading`
now pins the agreement all the way round the compass. **The M9's heading therefore changes
direction with this branch and wants re-checking against a known bearing.**

`src/sys/Compass.cpp` had been written against the QMC6309 — chip id
0x90 and that part's field layout — so the two parts are now one driver and two sets of
numbers in `src/sys/QmcMag.h`, chosen by `COMPASS_KIND`. The 6310's fields are QST's, taken
from the driver LilyGO ship for this board, and the configuration is the one their example
was running when it read this very part: continuous at 200 Hz, 8 G, no oversampling. The
counts that example printed beside its microtesla — 96 → 2.56 µT, −1280 → −34.13,
−4450 → −118.67 — are the vectors `test/test_qmc_mag` pins the scale against, so a range
field one value off fails on a host instead of quietly scaling every reading by four.

Two things are specific to this board. The heading is **not levelled**: tilt correction wants
gravity from the accelerometer, and this unit's is faulty, so every reading reports
`levelled=false` and a heading computed as if the board were flat — which for a pole-mounted
gateway it nearly is. And the part **does not follow the screen**: no page here shows a
bearing, so its readers are the console and the status API, which are asked of a dark node as
often as not (`COMPASS_FOLLOWS_SCREEN 0`). That is the same argument the accelerometer's rule
turns on, and it is now one board fact feeding one rule rather than two rules that happen to
agree — a compass left running in the dark also keeps the accelerometer up, because it still
wants gravity, which is why `IMU_FOLLOWS_SCREEN` is derived from it.

**And the accelerometer is fitted and does not answer.** Two earlier versions of this page
got that wrong in opposite directions, so here is what was measured rather than concluded.
The part is there: LilyGO's silkscreen names it, a chip sits under the name, and both their
wiki and their pin poster put a QMI8658 on the card's SPI bus at select 34.

| Question | What the bench says |
|---|---|
| Does the select work? | Yes — GPIO 34 reads back what it is driven to, and it is the only pin that changes anything: 33, 48, 21 and 14 each leave MISO at its pull-up |
| Is a part selected by it? | Yes — MISO has a stiff external pull-up and reads a hard 1 against an internal pull-down, then a hard 0 the moment 34 is asserted |
| What does it say? | Zeroes. Registers 0x00–0x0f, the status bytes, the temperature and 0x4d all read 0x00, but for a fixed 0x3e at the register-0 frame and 0x24 at the 0x0a frame |
| Is it the SPI mode or the clock? | No — the same bits in all four modes, at 1 MHz and 100 kHz, and identical when the clock is bit-banged by hand instead of by the peripheral |
| Do writes land? | No — CTRL1 written 0x40 reads back 0x00, and the vendor's reset (0xb0 into 0x60, then poll 0x4d for 0x80) never completes |
| Is it power? | No — every AXP2101 rail was read back and then every one switched on, including the three LilyGO's init raises and this firmware does not (BLDO2, DCDC4, DCDC5), and including their cold-boot power-cycle of the sensor rails. Not one byte changed |

The framing is not the difference either: SensorLib's SPI transport sends `reg | 0x80` with
the select low around it, which is `src/sys/Imu.cpp` byte for byte, and LilyGO's own board
init opens the same host on the same three pins with no enable line of its own.

That test has since been run, and it is the one that ends the argument. LilyGO's own
`QMI8658_GetDataExample`, built from their repository with their board definition, their
vendored SensorLib and their rail init — which power-cycles the sensor rails and raises
BLDO2, DCDC4 and DCDC5 — reports:

```
Found QMC6310N MAG Sensor at address 0x3C
Found OLED display at address 0x3D
Found BME280 Sensor at address 0x77
Sd Card init succeeded, The current available capacity is 7.44 GB
UBlox GNSS init succeeded, using UBlox GNSS Module
Failed to find QMI8658 - check your wiring!
```

Everything else on the board, found; the 6-axis part, not. **So it is a hardware fault on
this unit** — the part or its joints — and not something a firmware change can reach.
`HAS_IMU` stays 1 because the board carries the part and another unit's will answer; the
driver reports the absence honestly, with the byte it read.

Two things that run confirmed in passing. The **magnetometer is known good**, not merely
present: their `QMC63xx_GetDataExample` streams sensible fields from it on this unit (about
123 µT total, a steady heading, and a large Z offset, which is what their calibration
example is for). That is what made driving it a port rather than a research task, and it is
now driven — see the magnetometer section above. And this
unit is a **MAX-M10S**: their code probes for an L76K, fails, and says "UBlox GNSS init
succeeded". That is a fact about one board rather than the model, which is why `GPS_UBX`
stays 0 here — a UBX default would break every L76K unit sold.

**A/B over the air, on LilyGO's own numbers.** `partitions/ota_8mb.csv`: two 3264 KiB app
slots, a 1536 KiB filesystem and a 64 KiB coredump, closing the 8 MB part to the byte. The
numbers are not a compromise of ours — they are the table this board arrived carrying, read
off its factory Meshtastic image and kept in
`firmware-backups/tbeam-supreme-48CA435AD828.md`. A vendor shipping A/B on this part is
better evidence that it fits than any arithmetic, and it is the same kind of evidence the
4 MB table was argued from. Today's image is 58 % of a slot, against the 4 MB board's 94 %.

The filesystem is the one thing smaller than under the single-slot table it replaces —
1536 KiB where `huge_app_8mb.csv` gives 4.9 MB — and on this board that is the right trade:
it has a card slot, and the Reticulum store moves to the card when one is fitted. With no
card the store lives in the filesystem, and 1536 KiB is twelve times what the T3-S3 gets for
the same job.

A partition table is a one-way door. **A Supreme already flashed with the single-slot table
does not grow a second slot by taking an update** — it has to be written whole, once, over a
cable, and the node's own reported layout is what says which table it is on.

### T-Beam Supreme pin map
| Function | GPIO |
|---|---|
| SX1262 SCK / MISO / MOSI / CS | 12 / 13 / 11 / 10 |
| SX1262 RST / BUSY / DIO1 | 5 / 4 / 1 |
| SX1262 antenna switch | the chip's own DIO2; TCXO on DIO3 at 1.8 V |
| Panel I2C SDA / SCL (SH1106 @ **0x3d**, QMC6310 @ 0x3c, BME280 @ 0x77) | 17 / 18 |
| PMU I2C SDA / SCL (AXP2101, PCF8563 @ 0x51) — LilyGO's poster has these swapped | 42 / 41 |
| PMU IRQ (not driven) | 40 |
| GNSS RX / TX | 9 / 8 |
| GNSS PPS / L76K wake (neither driven) | 6 / 7 |
| microSD SCK / MISO / MOSI / CS | 36 / 37 / 35 / 47 |
| QMI8658 chip select (driven; the part is fitted and does not answer) / interrupt (not driven) | 34 / 33 |
| PCF8563 interrupt (not driven) | 14 |
| Button (BOOT) | 0 |
| Console | the chip's own USB (19 / 20) |

The power button is the PMU's own PWRKEY and does not reach the ESP32, so it cannot be read
as a GPIO — as on the Heltec V4's case button. Reserved: 26–32 (SPI flash and the quad
PSRAM), and 19/20 are the USB pads this board actually uses.

### What the bench settled, and what it did not

Brought up 2026-09-08 on the first flash of `tbeam-supreme`, over native USB. In the order
things had to work:

1. **The PMU answers, on its own bus.** `STATUS` reports `pmu=AXP2101`, and the two hosts
   read as they should: `i2c main sda=17 scl=18` with the panel and the BME280 on it, and
   `i2c host1 sda=42 scl=41` with exactly the PMU at 0x34 and the clock at 0x51. Every rail
   it feeds proved out by its consequence rather than by a register read — the radio
   answered (ALDO3), the panel initialised (ALDO1), the clock answered (ALDO2) and the card
   mounted (BLDO1).
2. **The radio is on air both ways.** SX1262 at 869.525, SF8: 24 packets received from the
   rest of the bench and 7 sent, rssi −34, snr 12.5, two CRC errors — ordinary LoRa. So the
   seven radio pins, the 1.8 V TCXO and DIO2 steering the antenna are all right. **Still
   open:** which of ALDO3 and DCDC3 the module actually needs. LilyGO's reference powers
   both, one as "the LoRa radio" and one as "the M.2 interface", and the radio works with
   both on — bring it up with DCDC3 off and see whether the transceiver still answers. A
   rail that costs current for nothing is worth knowing about on a board meant to run off
   an 18650.
3. **The panel works — and it is at 0x3d, which cost a bring-up to find.** Two addresses
   answer on that bus, 0x3c and 0x3d, and **both accept writes**. The firmware took 0x3c,
   which is where the probe's default order and the RNode firmware's block for this board
   both point; that device accepted the whole SH1106 initialisation and every frame after
   it without a single NAK, and the glass went on showing the frame Meshtastic had left in
   it. It reads as a hung display on a working node: `/api/status` says `display: true`,
   `STATUS` says `input panel=yes`, the driver's `begin()` returns true — and none of that
   means the glass is being driven. A register read-back *does* discriminate, but not in
   the direction anybody would guess, and reading one carelessly is how this bring-up
   convinced itself twice: 0x3c has a real register file — varied bytes, a chip id in
   register 0 — while 0x3d answers the same byte from every register, an SH1106 having no
   readable map at all. The address that looks like a working part is the one that is not
   the panel. What settled it was looking at the screen with the address pinned to 0x3d,
   where the pages render and the button steps through them; what identified the other was
   asking it for that chip id — 0x80, a QMC6310 magnetometer, in the census above.

   Two things follow for the next board. A probe that takes the first address to answer is
   only as good as the assumption that one address answers; and sending a command instead
   of a bare address query does not help, because the control byte and a NOP are a legal
   register write to any register chip — it discriminates nothing and puts a byte into a
   part the display code should not be touching.
4. **The card mounted and took the store.** An 8 GB SDHC on the second SPI host
   (`36/37/35`, select 47): `state: mounted`, `store_home: sd`, the Reticulum store at
   `/sd/rns`. So those four pins and BLDO1 are right.
5. **The clock is holding.** The PCF8563 answers at 0x51 on the PMU's bus and `STATUS`
   reads `rtc=holding drift=1s` — and it was already holding the right time before this
   firmware ever wrote it, the same as the M9's. So this board knows the date indoors.
6. **The receiver is talking.** 932 NMEA sentences and no fix, which is what indoors looks
   like — and it settles the two GNSS pins, which had only LilyGO's word behind them, along
   with the ALDO4 rail that has to be on before the part exists. **Still open:** a fix
   outdoors, and the cost of a nap. That last one is not inherited from the T-Beam, whose
   receiver keeps its almanac through a rail cut and comes back in seconds; this board's
   backup domain is the PMU's own VBACKUP, which nothing here switches and LilyGO's
   reference code turns off. A cold start makes the current fall too, so only the time to a
   fix after a nap separates the two — and if it is minutes, the duty cadence is paying more
   for a fix than it thinks.
7. **Native USB works, and the flashing route is not the obvious one.** The composite
   device enumerates as `RetiMesh Node` with the MAC in its serial, `usb0` reaches ready,
   and `USB_STATUS` reports `personality=usb_otg_composite ncm=driver software_entry=yes`.
   But **esptool cannot reset this board into its downloader over USB** — not the factory
   Meshtastic image and not ours; both answer `No serial data received`. What does work is
   the 1200-baud touch (`stty -F <port> 1200`), which makes the core jump to the ROM, after
   which the board appears under its `USB_JTAG_serial_debug_unit` name and esptool is happy.
   `pio run -t upload` gets there by itself through the console's `BOOTLOADER CONFIRM`;
   `uploadfs` does not, and needs the touch first. See
   `firmware-backups/tbeam-supreme-48CA435AD828.md`.
8. **The three parts on the panel's bus are each identified — and what this item first
   said was wrong.** It read: *the magnetometer is not fitted on this unit*, on the
   strength of a scan that found nothing at 0x0d, 0x1c, 0x2c or 0x7c, the addresses the
   M9's QMC6309 uses. The scan was right and the conclusion was not: the part had been
   answering at 0x3c, where a QMC6310N sits, from the first boot. It is fitted and it is
   undriven — `HAS_COMPASS 0` now stands on the register map rather than on an absent
   part, and `COMPASS_ADDR 0x3C` in the board header records the address so nobody scans
   for it again. The lesson is the cheap one: three parts answered and only two had been
   asked what they were.

Four more items come from the changes that drove the accelerometer, the BME280 and the
magnetometer, and one of them was a one-way door:

9. **The accelerometer — fitted, selected, silent, and still open.** `STATUS` reads
   `imu=no` and the boot log says `WHO_AM_I read 0x3e, wanted 0x05`. What that is *not* is a
   missing part: the table above is the bench work that settled the select, the drive on
   MISO, the modes, the clock rates, the hand-clocked control, the writes and every rail.
   The card mounts on those same wires in the same boot, so the bus, the pins and the
   shared-host arrangement are all good. **Closed by LilyGO's own example**, which finds
   every other part on the board and then says `Failed to find QMI8658 - check your
   wiring!` — so the fault is the part or its joints on this unit, and nothing a firmware
   change reaches. What is still open belongs to a unit whose part answers: that `imu=yes`
   survives the panel going dark. A reading of `asleep` there would mean
   `IMU_FOLLOWS_SCREEN` — `DISPLAY_AUTO_ROTATE || (HAS_COMPASS && COMPASS_FOLLOWS_SCREEN)`
   — had been derived wrongly, which on this board means the magnetometer had been made to
   follow the screen and taken the accelerometer with it.
10. **The magnetometer reads a heading — new, and unproven by this firmware.** LilyGO's
    example read this part before ours did, so the part and the numbers are known; what has
    never run on hardware is *our* driver against it. `STATUS` should show
    `compass heading=… levelled=no tilt=0 field=…uT cal=…%` and `/api/status` a matching
    `compass` object. Then: turn the node slowly through a full circle and watch `cal=`
    climb — it scores the worse of the two axes a bearing is computed from, so end-over-end
    tipping will not fill it. Turning the node **clockwise should make the heading
    increase**; if it decreases, the axis signs need register 0x29 (SIGN), which LilyGO's
    own driver declares and never writes. Field strength will read far above the Earth's
    25-65 µT until the offsets are learned — this board's own iron is about 120 µT of it.
    Two more: `compass=asleep` should **never** appear on this board (it does not follow the
    screen), and a restart should keep the calibration, since the extremes are held in NVS.
    And one that is easiest to check by *not* seeing it: a reading more than half a second
    old stops being offered, so if the part ever stops answering the console must say
    `compass=no-answer` rather than going on printing the last heading that worked. `age=`
    on that line should read 0 the whole time it is healthy.
11. **The BME280 reads plausibly — done.** 27.74 °C, 60.2 % and 1012.69 hPa on the first
    boot that had it. Still open, and the only one of the three with an independent
    reference: agreeing that pressure with a local weather station to within a couple of hPa,
    which is what would catch a transcription error in the polynomial that no host test can
    see.
12. **The A/B layout is installed — the update itself is not yet proved.** The board has been
    written whole over a cable and is running `partitions/ota_8mb.csv`, so the one-way door
    has been walked through. What has still never happened on this board is an update being
    installed into the other slot and booted from it, which is the half of A/B that matters.

One more thing the earlier bring-up turned up, which is not this board's fault: **two nodes on one
host can collide on the USB-NCM subnet.** This board came up as `usb0` at `10.64.40.1`,
which is the address the SX1280 T3-S3 already had — both MACs end in `0x28`, and the third
octet is derived from that last byte, so any two boards sharing it are indistinguishable by
address on the same host. It is not a fault in this port and is not fixed here.

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
Optional; hot-plug polled. An empty slot is looked at after 3 s, then 6, 12 and
24, and every 30 s from three quarters of a minute on: asking an empty slot
costs the driver about half a second each time, so a node that has been sitting
with nothing in it stops paying for the answer. Two things put it back on the
3 s beat and nothing else does: the slot's own answer changing — a card turns
up, or a mounted one is lost — and the node's button being pressed, which also
makes it look straight away. So a card put into a node that has been up for a
while is found within 30 s, or at once if you press the button after inserting
it. A mounted card is touched on the same 30 s beat — that touch is the removal
check, so a card that has been pulled out can be reported as still present for
up to half a minute. A card that is present but will not mount is looked at
every 3 s for the first minute and a half and then backs off the same way;
asking for a format ends the wait whatever it has reached, so a format still
starts as soon as it is asked for. The card is mounted as one FAT volume at
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
GNSS (where a receiver is fitted) → weather (where a BME280 is) → QR, with the
current one marked by the dots at the bottom right. Short press: next page (wakes the panel first if
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

## The radio self-test, and why it stops appearing

Boards whose interrupt pin was in doubt when they were brought up transmit one
short frame at boot and time the TxDone interrupt back — the Heltec V3, V4,
Wireless Bridge, Wireless Paper and Wireless Stick, the two SX1280 T3-S3
variants and the ThinkNode M9. A chip answers SPI, reports itself online and
accepts a channel whatever pin the interrupt is on, so this is the only thing
that separates a working board from one that will never receive a packet:

```
radio self-test: TxDone interrupt arrived in 7 ms — the IRQ line on GPIO 14 is live
```

(GPIO 14 is the SX1262 interrupt on the three Heltec S3 boards; the Wireless
Stick and Bridge print 26, the SX1280 T3-S3 variants 9 and the ThinkNode M9 42.)

**It runs once per firmware image, not once per boot.** A pass is written to
NVS against the running binary — the SHA-256 of the ELF it was built from, out
of the application descriptor, rather than the version string it prints — and
later boots of that same image say so and skip the transmission:

```
radio self-test: skipped — this exact image already proved the IRQ line on GPIO 14
```

That is a power decision rather than a tidiness one. A solar node whose battery
is flat at dawn brown-out loops, and the self-test spends a transmission on
every cycle out of the supply that could not hold the last boot up. What the
test proves is the board header's pin map, which belongs to the image — so
flashing any different firmware asks the question again, which is also exactly
when the answer can have changed. That is why the marker is keyed to the binary
and not to `FW_VERSION`: a local build takes its version from `git describe
--always --dirty`, and every build made while the tree stays dirty carries the
same string. Keyed to the string, the reflash after a pin-map edit would find
the previous image's marker and announce a line it had never driven — during
bring-up, which is the one situation this test exists for. A **failure is never
recorded**: a board with a wrong interrupt pin keeps saying so, every boot,
until it is fixed.

The marker lives in its own NVS namespace, so neither a settings reset nor
clearing the diagnostics history touches it.

**"Rebuild it" is not a way to force one more run.** The identity is the image,
so re-running the build only asks the question again if the image that comes out
is a different one, and on an unchanged tree it is not: `pio run` finds nothing
to do, relinks nothing and leaves the previous `firmware.bin` in place, byte for
byte. Touching a file does not change that either — PlatformIO decides what is
stale from file contents, not timestamps. What does produce a new image is any
edit that reaches the binary, and that includes the version stamp: a local build
takes `FW_VERSION` from `git describe --always --dirty`, so building with an
uncommitted change in the tree is already a different image from the same commit
built clean. Failing all that, erase the flash (`--erase-all`, see
[getting-started.md](getting-started.md)) — the marker goes with the rest of NVS
and the next boot starts from nothing.

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
