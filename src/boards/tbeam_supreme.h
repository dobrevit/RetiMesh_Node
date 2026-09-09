// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd
//
// This file is part of RetiMesh Node.
//
// RetiMesh Node is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// RetiMesh Node is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with RetiMesh Node. If not, see <https://www.gnu.org/licenses/>.

#pragma once
// LilyGO T-Beam Supreme (T-Beam S3 Supreme, V3.0) — the T-Beam's successor:
// ESP32-S3FN8 with 8 MB of flash and 8 MB of quad PSRAM, an SX1262 in a
// socketed module, an AXP2101, a u-blox MAX-M10S or Quectel L76K receiver, a
// 1.3" SH1106 panel, a microSD slot, an 18650 holder and a shelf of sensors.
//
// It shares a name with boards/tbeam.h and almost nothing else. The classic
// T-Beam is a classic ESP32 behind a CH9102 bridge with the radio, the GPS and
// the OLED on one PMU-fed I2C bus and no card slot; this is an S3 with native
// USB, two I2C buses, two SPI buses and six switched rails. What does carry
// over is the shape of the problem: nothing on the board answers until the PMU
// has been told to power it, so Pmu::begin() runs before the radio is probed
// and an unpowered rail reads exactly like a part that is not fitted.
//
// Where these numbers come from
// -----------------------------
// Two sources, one of which is a firmware running on this exact board:
//
//   * LilyGO's own hardware documentation for the T-Beam Supreme
//     (wiki.lilygo.cc), which names the parts and both I2C buses;
//   * the RNode firmware's board block for it (BOARD_TBEAM_S_V1 in Boards.h,
//     Power.h and Display.h in the ../RNode_Firmware mirror).
//
// They agree, pin for pin, on everything that decides whether this board comes
// up: the radio's seven lines, the card's four, the PMU's bus and interrupt,
// the panel's bus and its controller, the accelerometer's select, the button,
// and the TCXO voltage. Worth saying which lines have only the one source,
// because that is where to look first if something does not answer: the GNSS
// UART, its 1PPS output and the L76K wake line, the clock's interrupt and the
// accelerometer's interrupt are LilyGO's numbers alone — RNode drives no
// receiver, no clock and no accelerometer on this board, so it says nothing
// about them either way. None of those five is on the boot path.
//
// The rail map is LilyGO's own reference code, copied into RNode's Power.h and
// reproduced below. It is the part with no room for inference: this board's
// AXP2101 feeds the transceiver from a different regulator than the T-Beam's
// does, and the T-Beam's map applied here powers a sensor rail and leaves the
// radio dark.
//
// Brought up on hardware 2026-09-08 and the map held: the radio is on air both
// ways, the card mounted, the clock was already holding the right time, the
// receiver is talking, and the panel draws its pages and steps through them on
// the button — once it was pointed at the right address, which took a bring-up
// of its own and is the note under OLED_ADDR below. docs/hardware.md carries
// what the bench settled and the one thing it did not: what a rail-cut nap
// costs the receiver here.

// The PSRAM is external rather than in-package: esptool's feature list for this
// die names 8 MB of embedded *flash* and no PSRAM at all, where an R2/R8 part
// names both. It is nonetheless there and usable — the node reports 7.9 MB of it
// free — so the env's BOARD_HAS_PSRAM is right, and the portal can run here.
#define BOARD_NAME          "LilyGO T-Beam Supreme"

// ---------------------------------------------------------------------------
// Radio — SX1262 on a module in a socket
// ---------------------------------------------------------------------------
// The transceiver is not soldered to this board: it is a plug-in module, which
// is why the rail that feeds it and the socket's own supply are separate
// entries in the rail map below. Electrically it is the same SX1262 as the
// T3-S3's and the T-Deck's — a TCXO on DIO3 at 1.8 V, DIO2 driving the antenna
// switch, no external amplifier — so the probe and every RF default apply
// unchanged.
#define PIN_LORA_SCK        12
#define PIN_LORA_MISO       13
#define PIN_LORA_MOSI       11
#define PIN_LORA_CS         10
#define PIN_LORA_RST        5
#define PIN_LORA_BUSY       4
#define PIN_LORA_DIO1       1
// No SX127x is offered on this board, and the probe needs the pin to be a pin
// it can leave alone rather than one it might drive.
#define PIN_LORA_DIO0       -1

// The transceiver has a bus to itself; the card and the accelerometer share
// the other one. On the S3, FSPI and HSPI are both general-purpose.
#define LORA_SPI_BUS        FSPI

// Stated rather than inherited because both are RF facts with a bench cost if
// they are wrong: a TCXO given the wrong voltage does not start, and a radio
// whose antenna switch is not steered answers over SPI and says nothing on the
// air. Both are the mirror's values for this board.
#define RF_TCXO_VOLTAGE     1.8
#define RF_DIO2_AS_SWITCH   true
#define HAS_PA              0

// ---------------------------------------------------------------------------
// Two I2C buses, and which parts are on which
// ---------------------------------------------------------------------------
// The first board here where the power-management chip is not on the panel's
// bus. LilyGO splits them:
//
//   bus 0 (SDA 17, SCL 18)   the SH1106 panel, the BME280, the magnetometer
//   bus 1 (SDA 42, SCL 41)   the AXP2101 and the PCF8563 clock
//
// So the general pair below is the panel's, and the PMU and the clock name
// their own pins. I2cReg::busFor() is what turns a pair of pins into a host —
// one place deciding, because two drivers reaching for Wire1 separately would
// each start it on their own pins and the second would move the first's device
// onto the wrong wires (src/sys/I2cReg.h).
#define PIN_OLED_SDA        17
#define PIN_OLED_SCL        18
// PIN_I2C_SDA/SCL default to the panel's pair, which is right here: the
// sensors that are not yet driven are on that bus, and `I2C` on the console
// enumerates it.

// ---------------------------------------------------------------------------
// Display — 1.3" SH1106, not the SSD1306 every other OLED board here carries
// ---------------------------------------------------------------------------
// Same 128x64 geometry and the same GFX calls as every other OLED board here,
// and a different controller: the SH1106 has 132 columns of RAM with the panel
// wired to the middle 128, and no horizontal addressing mode. Driven by the
// SSD1306 code it comes up shifted by two columns, wrapping the last two, with
// an initialisation sequence it only partly understands. The address is not
// shared — see OLED_ADDR below. See src/ui/OledPanel.h.
#define HAS_DISPLAY         1
#define OLED_CONTROLLER     OLED_CONTROLLER_SH1106
// 0x3D, measured on the bench, and stated here because nothing can work it out
// at runtime: two addresses answer on this bus — 0x3c and 0x3d — and both take
// a write. Taking 0x3c, which is where the default order and the RNode
// firmware's block for this board both point, got a device that swallowed an
// entire SH1106 initialisation and every frame after it without one NAK, while
// the glass went on showing the picture the previous firmware had left in it.
// The panel is the other one. What lives at 0x3c is the magnetometer — see the
// section on it below — which explains why it took the writes: a register file
// accepts any address you send it.
//
// This is the board fact that cost the most to find, so: `display: true`,
// `input panel=yes` and a successful driver `begin()` prove only that something
// accepted the initialisation. On a bus with two answers they do not prove the
// glass is being driven, and no register read-back settles it either — both
// addresses return the same bytes no matter what is written to them. The only
// instrument that worked was looking at the screen.
#define OLED_ADDR           0x3D
// Reset is not wired to a GPIO on this board; the panel comes out of reset with
// its rail.
#define PIN_OLED_RST        -1

// ---------------------------------------------------------------------------
// Power — an AXP2101 that owns almost everything
// ---------------------------------------------------------------------------
// Six switched rails, of which five have to be on before the parts behind them
// exist as far as this firmware is concerned. The names are LilyGO's:
//
//   ALDO1  the panel, the 6-axis part, the magnetometer and the BME280
//   ALDO2  the sensor bus itself and the PCF8563 — LilyGO's code notes this
//          one cannot be turned off without losing the clock
//   ALDO3  the transceiver module
//   ALDO4  the GNSS receiver (off at power-up, and left off here)
//   BLDO1  the microSD slot
//   DCDC3  the module socket
//
// DCDC1 is the system rail this chip is powering the ESP32 from, so it is left
// alone: on the T-Beam it is the display's rail and gets switched on, and here
// switching it is switching the board off.
//
// LilyGO's pin poster (assets/image/T-BEAM-S3-Supreme.jpg) has the next two
// the other way round — "PMU SDA: IO41, SCL: IO42" — and it is wrong, which
// is worth writing down because it is the kind of error that cannot be half
// right: SDA and SCL are not interchangeable, and the AXP2101 and the clock
// both answer with them this way round. The same poster puts the RTC on the
// panel's bus at 17/18; the PCF8563 answers at 0x51 on this one. Take that
// poster for the parts list and this header for the wiring.
#define HAS_PMU             1
#define PIN_PMU_SDA         42
#define PIN_PMU_SCL         41
// Recorded rather than driven, as on the T-Beam: nothing here reads the PMU's
// interrupt line yet. It is the pin to reach for when the power button and the
// charge/insert events are wanted.
#define PIN_PMU_IRQ         40
// The PMU is the battery reader on this board; there is no divider to sample.
#define HAS_BATTERY_ADC     0

#define PMU_RAIL_RADIO      XPOWERS_ALDO3
#define PMU_RAIL_GPS        XPOWERS_ALDO4
#define PMU_RAIL_DISPLAY    XPOWERS_ALDO1
#define PMU_RAIL_SENSORS    XPOWERS_ALDO2
#define PMU_RAIL_CARD       XPOWERS_BLDO1
#define PMU_RAIL_MODULE     XPOWERS_DCDC3

// ---------------------------------------------------------------------------
// GNSS — u-blox MAX-M10S or Quectel L76K, depending on the unit
// ---------------------------------------------------------------------------
// LilyGO ship this board with either receiver and the same footprint, and
// nothing on the wire says which is fitted until it starts talking: both come
// up at 9600 baud speaking NMEA. So GPS_UBX stays off. A UBX frame sent to an
// L76K is a message that part has never heard of, and the only thing the
// binary protocol would buy here is a nap this board does not need it for —
// the PMU can cut the receiver's rail outright, which is what GPS_NAP derives
// to (Config.h) and a harder off than any message.
//
// If a unit is confirmed to carry the MAX-M10S, GPS_UBX 1 is the one line that
// changes, and it buys the UBX position and time frames rather than the nap.
// The unit this was brought up on *is* a MAX-M10S, confirmed the only way that
// counts: LilyGO's own example probes for an L76K, fails, and reports "UBlox
// GNSS init succeeded, using UBlox GNSS Module". That is a fact about one
// board and not about the model, which is exactly why the default here stays
// NMEA — a UBX default would break every L76K unit sold.
//
// One cost to know about that rail cut: the receiver's backup domain here is
// the AXP2101's own VBACKUP, which nothing names and nothing switches — and
// LilyGO's reference code for this board turns it off. So unlike the T-Beam,
// whose receiver keeps its almanac through a nap, a wake on this board should
// be assumed to be a cold start until a bench times one. Naming VBACKUP as a
// rail and enabling it is the fix if it matters, and it is worth a current
// measurement rather than an assumption.
#define HAS_GPS             1
#define PIN_GPS_RX          9                // the S3 receives here
#define PIN_GPS_TX          8
#define GPS_BAUD            9600
#define GPS_UBX             0
// Two lines the board wires and this firmware does not use, recorded so the
// next person does not have to find them again: the 1PPS output is on GPIO 6,
// and GPIO 7 is the L76K's wake line — absent on a MAX-M10S unit, which is why
// it is not PIN_GPS_STANDBY. Naming it would make the nap a pin write that
// does nothing on half the units sold, instead of the rail cut that works on
// all of them.

// ---------------------------------------------------------------------------
// microSD — its own bus, shared with the accelerometer
// ---------------------------------------------------------------------------
// The card is on the second SPI host with the QMI8658's chip select beside it,
// and both are driven — they ask SpiBus for the same object rather than
// constructing one each, because two SPIClass objects on one host re-initialise
// the peripheral under each other (src/sys/SpiBus.h). BoardInit idles both
// selects before either driver starts, since the accelerometer's begin() runs
// first and a floating select is a device that may answer.
#define HAS_SD              1
#define PIN_SD_SCK          36
#define PIN_SD_MISO         37
#define PIN_SD_MOSI         35
#define PIN_SD_CS           47
#define SD_SPI_BUS          HSPI

// ---------------------------------------------------------------------------
// Clock — PCF8563 on the PMU's bus
// ---------------------------------------------------------------------------
// The same part the ThinkNode M9 carries, at the same fixed address, on the
// PMU's pins rather than the general pair. It is what gives this board correct
// time indoors with no fix, where a board without one counts from 1970 until
// the sky clears — and it is fed from ALDO2, which is why that rail is on
// before anything asks the clock the time. Its interrupt is on GPIO 14 and
// nothing reads it.
#define HAS_RTC             1
#define PIN_RTC_SDA         PIN_PMU_SDA
#define PIN_RTC_SCL         PIN_PMU_SCL

// ---------------------------------------------------------------------------
// Accelerometer — a QMI8658, and the first one here that is not on I2C
// ---------------------------------------------------------------------------
// The same part the ThinkNode M9 carries, wired to the card's SPI bus with a
// select of its own instead of to a shared I2C bus. That is a transport and
// not a different part: every register, the chip id and the configuration are
// the M9's, and only the three calls at the top of src/sys/Imu.cpp change
// (IMU_TRANSPORT). The bus and its three pins default to the card's, which is
// where they are — so the select is all this board has to name.
//
// Two consequences worth having written down. BoardInit idles this select
// *and the card's* before either driver exists, and depends on neither of them
// going first: a floating select is a device that may answer, and which
// begin() gets there first is a thing to get wrong rather than a thing to rely
// on — as an earlier draft of this comment did, naming the card, when
// main.cpp has Imu::begin() at 267 and sdCard.begin() at 277. And the part
// does *not* follow the screen here, unlike on the board that had one first:
// nothing on this board reads it for panel rotation, and the magnetometer that
// would want gravity from it is not suspended with the glass either, so its
// only readers are the console and the status API — which are asked with the
// glass dark as often as not. A derivation rather than a board opinion:
// IMU_FOLLOWS_SCREEN is DISPLAY_AUTO_ROTATE || (HAS_COMPASS &&
// COMPASS_FOLLOWS_SCREEN), and the pair below sets the second half to 0
// (Config.h, PeripheralPolicy.h). Turn COMPASS_FOLLOWS_SCREEN on and this
// follows it, which is the point of deriving it rather than stating it.
//
// **The part is fitted on the unit this was brought up on and does not answer
// over SPI.** Read that carefully, because two earlier versions of this
// comment got it wrong in opposite directions. The part is there: LilyGO's
// silkscreen names it, a chip sits under the name, and their wiki and pin
// poster both put a QMI8658 on the card's SPI bus at select 34. What it will
// not do is talk.
//
// What was measured, so nobody has to take it on trust:
//
//   * the select works — GPIO 34 reads back what it is driven to, and it is
//     the *only* pin that changes anything: 33, 48, 21 and 14 each leave MISO
//     at its pull-up;
//   * something is selected by it — MISO carries a stiff external pull-up and
//     reads a hard 1 against an internal pull-down, and goes to a hard 0 the
//     moment 34 is asserted, so a part is there and driving the line;
//   * and it reads as zeroes. Every register in 0x00-0x0f, the status bytes,
//     the temperature and 0x4d come back 0x00, but for a fixed 0x3e at the
//     register-0 frame and 0x24 at the 0x0a frame — the same bits in all four
//     SPI modes, at 1 MHz and at 100 kHz, and identical when the clock is
//     bit-banged by hand instead of by the peripheral;
//   * no write lands: CTRL1 written 0x40 reads back 0x00, and the vendor's own
//     reset — 0xb0 into 0x60, then poll 0x4d for 0x80 — never completes;
//   * and it is not power. Every AXP2101 rail was read back on the bench and
//     then every one of them switched on, including the three LilyGO's init
//     brings up and this firmware does not (BLDO2, DCDC4, DCDC5), and
//     including their cold-boot power-cycle of the sensor rails. Not one byte
//     changed.
//
// The framing is not the difference either: SensorLib's SPI transport sends
// `reg | 0x80` with the select low around it, which is this driver byte for
// byte, and their board init opens the same host on the same three pins with
// no enable line of its own.
//
// And that test has now been run. LilyGO's own QMI8658_GetDataExample, built
// from their repository with their board definition, their vendored SensorLib
// and their rail init — which power-cycles the sensor rails and brings up
// BLDO2, DCDC4 and DCDC5 — prints:
//
//     Found QMC6310N MAG Sensor at address 0x3C
//     Found OLED display at address 0x3D
//     Found BME280 Sensor at address 0x77
//     Sd Card init succeeded, The current available capacity is 7.44 GB
//     UBlox GNSS init succeeded, using UBlox GNSS Module
//     Failed to find QMI8658 - check your wiring!
//
// Everything else on the board, found. The 6-axis part, not. So this is a
// hardware fault on this unit — the part or its joints — and not something a
// firmware change can reach. HAS_IMU stays 1 because the *board* carries the
// part and another unit's will answer; this driver reports the absence
// honestly, with the byte it read, which is all it can do.
#define HAS_IMU             1
#define IMU_KIND            IMU_KIND_QMI8658
#define IMU_TRANSPORT       IMU_TRANSPORT_SPI
#define PIN_IMU_CS          34
// The panel does not turn: 128x64 pages laid out one way, in a fixed case. So
// the accelerometer answers the console and nothing else, and the rotation
// path is not built (it defaults to HAS_IMU, which is now 1).
#define DISPLAY_AUTO_ROTATE 0
// The interrupt is on GPIO 33 and nothing reads it: this part is polled. It is
// the pin to reach for if a motion wake is ever wanted.
//
// The axis signs are the defaults, which is to say unverified — how the part
// sits relative to the case has not been measured, and nothing here depends on
// it yet. A consumer that cares (a heading, a rotation) has to settle
// IMU_INVERT_X/Y/Z on the bench first.

// ---------------------------------------------------------------------------
// Environment — a BME280 on the panel's bus
// ---------------------------------------------------------------------------
// Temperature, pressure and humidity, at 0x77: SDO is strapped high here,
// where the same part on another board is as likely to be at 0x76. It shares
// the panel's bus, which is why Environment::poll() is called from the main
// loop and from nowhere else — one reader on that bus is what keeps it safe
// (Environment.h, and issue 33 in the roadmap for the reason).
//
// This is the first sensor here that reports on the node's surroundings rather
// than on the node, and the first that does not follow the screen: a gateway
// on a pole reports the weather at the pole whether or not anybody is looking
// at its glass.
#define HAS_ENV             1
#define ENV_KIND            ENV_KIND_BME280
#define ENV_ADDR            0x77

// ---------------------------------------------------------------------------
// Magnetometer — a QMC6310N, and it took some finding
// ---------------------------------------------------------------------------
// It is at **0x3c**, on the panel's bus, and it reports chip id 0x80 in
// register 0 — a QMC6310, not the QMC6309 the M9 carries and not at any of the
// addresses that part uses. An earlier version of this header declared it
// absent on the strength of a scan that found nothing at 0x0d, 0x1c, 0x2c or
// 0x7c, which was the wrong conclusion from the right data: nobody had asked
// what the *third* answer on the panel's bus was.
//
// It is also, and this is worth writing down once, the device that made the
// panel look broken. Two addresses answered on that bus; 0x3c was taken for
// the panel, accepted an entire SH1106 initialisation and every frame after it
// without complaint — because a magnetometer's registers will take any write
// — while the glass, at 0x3d, kept the previous firmware's picture.
//
// It is driven, and the numbers it is driven with are not guesses. The part is
// known good on this unit before any of this firmware touched it: LilyGO's own
// QMC63xx_GetDataExample read it here and streamed sensible fields — about
// 123 uT total with a large offset on Z, which is what their calibration
// example exists for — in continuous mode at 200 Hz, 8 G, no oversampling.
// Those are the register values src/sys/QmcMag.h calls verified, and the
// counts that example printed beside its microtesla are the test vectors that
// pin this board's scale (test/test_qmc_mag).
//
// Two things about the reading on this board specifically.
//
// It is not levelled, and it says so. Tilt correction wants gravity from the
// accelerometer, and the 6-axis part above is faulty on this unit — so every
// reading here comes back with `levelled=false` and a heading computed as if
// the board were flat, which for a gateway bolted to a pole it very nearly is.
// A unit whose accelerometer answers gets the corrected heading with no change
// here.
//
// And it does not follow the screen. Nothing on this board's 128x64 pages
// shows a bearing: the readers are the console and the status API, which are
// asked of a dark node as often as not, so suspending the part with the glass
// would hand "asleep" to every caller it has. That is the same argument the
// accelerometer's own rule turns on, and it is stated once as a board fact
// rather than twice as a rule (COMPASS_FOLLOWS_SCREEN, PeripheralPolicy.h) —
// which also keeps IMU_FOLLOWS_SCREEN honest, since a compass that runs in the
// dark would otherwise hold up an accelerometer that had gone to sleep.
#define HAS_COMPASS         1
#define COMPASS_KIND        COMPASS_KIND_QMC6310
// Recorded so the next reader does not repeat the scan: 0x3c is the
// magnetometer, 0x3d is the panel, 0x77 is the BME280.
#define COMPASS_ADDR        0x3C
#define COMPASS_FOLLOWS_SCREEN 0

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------
// The BOOT key, which is this board's only GPIO button — the power button is
// the PMU's own PWRKEY and does not reach the ESP32.
#define PIN_BUTTON          0
