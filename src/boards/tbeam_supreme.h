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
// Not verified on hardware yet. Everything below is documented rather than
// measured, and docs/hardware.md carries the bench list that settles it.

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
// Same 128x64 geometry and the same address, and a different controller: the
// SH1106 has 132 columns of RAM with the panel wired to the middle 128, and no
// horizontal addressing mode. Driven by the SSD1306 code it comes up shifted
// by two columns, wrapping the last two, with an initialisation sequence it
// only partly understands. See src/ui/OledPanel.h.
#define HAS_DISPLAY         1
#define OLED_CONTROLLER     OLED_CONTROLLER_SH1106
#define OLED_ADDR           0x3C
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
// The card is on the second SPI host with the QMI8658's chip select beside it.
// Nothing drives the accelerometer yet; when something does, it asks
// SpiBus::get(SD_SPI_BUS, ...) for the same object rather than constructing
// one of its own — two SPIClass objects on one host re-initialise the
// peripheral under each other (src/sys/SpiBus.h).
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
// Fitted, not driven
// ---------------------------------------------------------------------------
// Three parts this board carries that this firmware does not read yet. Each is
// off for a stated reason rather than for want of a pin:
//
//   * QMI8658 6-axis (chip select GPIO 34, interrupt GPIO 33) — the driver in
//     src/sys/Imu.cpp speaks to this part over I2C, and here it is on SPI,
//     sharing the card's bus. A transport, not a pin map.
//   * QMC6310 or QMC6309 magnetometer on the panel's bus — src/sys/Compass.cpp
//     is written against the QMC6309 the M9 carries, identified by a chip id of
//     0x90 and a register map read off that part. Which of the two is fitted
//     here, and whether its registers agree, is a bench question: `I2C` on the
//     console enumerates the bus and `I2C <addr>` dumps the part, which is how
//     the M9's was settled before a line of driver was written.
//   * BME280 on the panel's bus — no environmental sensor exists anywhere in
//     this firmware. Temperature, humidity and pressure would want a driver, a
//     capability flag, and a place on the status API, the console, the display
//     and in docs/, which is a feature rather than a board port.
#define HAS_IMU             0
#define HAS_COMPASS         0

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------
// The BOOT key, which is this board's only GPIO button — the power button is
// the PMU's own PWRKEY and does not reach the ESP32.
#define PIN_BUTTON          0
