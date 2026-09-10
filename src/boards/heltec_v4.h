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
// Heltec V4 (TFT) — ESP32-S3 with an SX1262 behind an amplified front end, a
// 240x320 colour panel with a capacitive touch layer, two buttons, a sounder,
// a battery gauge and a GNSS receiver on the expansion header.
//
// The richest board this firmware has run on, and the first with a front end
// that has to be driven before the radio can hear anything at all.
//
// Where these numbers come from
// -----------------------------
// The open-source firmware the board ships with, whose board definition for
// this exact model (`variants/esp32s3/heltec_v4`, the TFT build) is the only
// published pin map there is: no Arduino variant carries this board. That is
// one source rather than the two this project prefers, so each group below is
// marked with how much weight it can carry:
//
//   * the LoRa pins are the same as the V3's, a board this bench has verified,
//     and the V4 keeps that module's wiring — two sources agreeing;
//   * the panel, touch, button and sounder pins had that one source only,
//     until the bench proved all but the sounder;
//   * the front end is described twice there, once as a comment on the wiring
//     and once in the code that drives it, and the two agree.
//
// Pin assignments are facts about the circuit rather than anything authored,
// so nothing is borrowed here but the facts: every line of the driver below
// and in src/radio/LoRaFem.cpp is this project's own.
//
// Bench-verified 2026-09-02: radio both directions through the front end,
// GNSS, panel, touch, both buttons. boards.json carries the status — the one
// copy of it — and docs/hardware.md the story; the sounder alone stayed
// silent and may not be fitted.

#define BOARD_NAME          "Heltec V4 TFT"

// ---------------------------------------------------------------------------
// Radio — SX1262, the same wiring as the Heltec V3
// ---------------------------------------------------------------------------
#define PIN_LORA_SCK        9
#define PIN_LORA_MISO       11
#define PIN_LORA_MOSI       10
#define PIN_LORA_CS         8
#define PIN_LORA_RST        12
#define PIN_LORA_BUSY       13
#define PIN_LORA_DIO1       14               // SX1262 IRQ
#define PIN_LORA_DIO0       -1               // no SX127x here
#define LORA_SPI_BUS        FSPI

#define RF_TCXO_VOLTAGE     1.8
// DIO2 still steers the transmit/receive path, as on the V3 — but here it does
// it through the front end's own path pin rather than through a bare switch.
// The chip does not know the difference, and neither does this setting.
#define RF_DIO2_AS_SWITCH   true

// The front end. This is the part that has no equivalent on any other board
// here: an SX1262 wired straight to an antenna works with nothing driven,
// while this one is deaf and mute until the amplifier is powered and told
// which way to face. See src/radio/LoRaFem.h for the sequence and for why the
// board is asked at boot which of two parts it carries.
#define HAS_LORA_FEM        1
#define PIN_FEM_POWER       7                // LDO enable for the FEM rail
#define PIN_FEM_ENABLE      2                // CSD on both parts: high to run
#define PIN_FEM_MODE_GC1109 46               // CPS: high = full PA, low = bypass
#define PIN_FEM_MODE_KCT    5                // CTX: high = TX / RX bypass, low = RX LNA

// An amplifier is fitted, which this firmware reports rather than compensates
// for: RF_TX_DBM is what the chip is driven at, here as on the SX1280+PA
// board, and the operator accounts for the gain. That gain is not a constant —
// the part compresses, so it falls from about 11 dB at low drive to 7 dB at
// the top (GC1109; the KCT8103L starts at 13 dB). The tables are in LoRaFem.h.
//
// The default drive is left at the firmware's usual 7 dBm, which through this
// front end leaves the antenna at roughly 18 dBm. That is inside the 500 mW
// allowance of the 869.4-869.65 MHz sub-band the default channel sits in, and
// it is over the 25 mW allowed in most of the rest of EU868 — so a channel
// change on this board is a power decision as well as a frequency one.
#define HAS_PA              1

// A transmit at boot, timed against the interrupt. Worth more here than on any
// board so far: with the front end unpowered the chip still answers over SPI
// and still reports itself online, and the failure is silence.
#define RADIO_SELFTEST_ON_BOOT 1

// ---------------------------------------------------------------------------
// Display — 240x320 ST7789 over SPI, and a capacitive touch layer on I2C
// ---------------------------------------------------------------------------
#define HAS_DISPLAY         1
#define DISPLAY_KIND        DISPLAY_KIND_TFT
#define DISPLAY_WIDTH       240
#define DISPLAY_HEIGHT      320

// SPI3 (HSPI), because the radio has FSPI and the two must not share a bus:
// the panel is written in long bursts and the radio's timing is not something
// to make wait behind them.
#define PIN_TFT_SCK         17
#define PIN_TFT_MOSI        33
#define PIN_TFT_MISO        -1               // write-only panel, three-wire
#define PIN_TFT_DC          16
#define PIN_TFT_CS          15
#define PIN_TFT_RST         18
#define PIN_TFT_BL          21               // backlight, active high
#define TFT_SPI_BUS         HSPI
#define TFT_SPI_HZ          40000000

// The touch layer is a CHSC6X on its own I2C bus, away from everything else.
#define HAS_TOUCH           1
#define PIN_TOUCH_SDA       47
#define PIN_TOUCH_SCL       48
#define PIN_TOUCH_RST       44
// The published map has the interrupt line commented out at 45 and passes -1
// instead, so the controller is polled rather than trusted to raise a line
// nobody has confirmed is connected. Polling a touch panel a few times a
// second costs one I2C transaction and cannot be wrong about a wire.
#define PIN_TOUCH_INT       -1
#define TOUCH_ADDR          0x2E

// The board's general-purpose I2C, and the case's two residents on it: the
// BQ25896 charge manager whose /QON pin is what the physical power button
// presses (the schematic shows the bare module has only RESET and USER — the
// power button belongs to the case), and the DA217 accelerometer that
// tells the panel which way up it is being held.
#define PIN_I2C_SDA         4
#define PIN_I2C_SCL         3
#define HAS_BQ25896         1
#define HAS_IMU             1                // a DA217; the panel follows the hand
#define IMU_KIND            IMU_KIND_DA217

// No OLED here, but the display module's I2C names still have to resolve.
#define PIN_OLED_SDA        PIN_I2C_SDA
#define PIN_OLED_SCL        PIN_I2C_SCL
#define PIN_OLED_RST        -1

// Vext switches the peripheral rail, active low, as on every Heltec board.
#define HAS_DISPLAY_VEXT    1
#define PIN_DISPLAY_VEXT    36

// ---------------------------------------------------------------------------
// Buttons, sounder, LEDs
// ---------------------------------------------------------------------------
#define PIN_BUTTON          0                // PRG/BOOT, the navigation key
// A second button on the case. The OLED version of this board uses the same
// pin for an LED instead, which is worth knowing before borrowing anything
// from that board's map.
#define PIN_BUTTON2         35
#define HAS_BUTTON2         1

// A piezo sounder on a PWM channel — not an audio path, and not on the board
// itself: the pin runs to an expansion slot, and the sounder is one of the
// modules that plug into it. The bench unit's silence at every drive was a
// slot with nothing in it. Driving the pin costs nothing without a module
// and is what makes one work the moment it is plugged in.
#define HAS_BUZZER          1
#define PIN_BUZZER          6

#define PIN_STATUS_LED      -1

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
#define HAS_PMU             0
#define HAS_SD              0

// The cell is read through a divider that is only connected while GPIO 37 is
// held high — the same arrangement the V3 has and the reason the V3 reports no
// battery at all. Reading the pin without raising the control line returns a
// plausible number that is not the battery, which is why the enable is part of
// the board description rather than something the reader assumes.
//
// The ratio is not the usual halving: this board divides deep and trims the
// result, 4.9 * 1.045, and reads it at 2.5 dB attenuation rather than the 11 dB
// the other boards use — which is what a divider this deep needs to keep the
// reading inside the converter's range.
#define HAS_BATTERY_ADC     1
#define PIN_BATTERY_ADC     1
#define PIN_BATTERY_ADC_EN  37
#define BATTERY_DIVIDER_RATIO (4.9f * 1.045f)
#define BATTERY_ADC_ATTEN   ADC_2_5db

// ---------------------------------------------------------------------------
// GNSS — on the expansion kit, not on the bare board
// ---------------------------------------------------------------------------
// The published pin map is explicit that the receiver (9600 baud) is part of
// the V4 expansion kit rather than of the board itself. This build assumes the
// kit is fitted, because the bench unit has one; a bare V4 wants HAS_GPS 0.
#define HAS_GPS             1
#define PIN_GPS_RX          39               // ESP32 receives here
#define PIN_GPS_TX          38
#define GPS_BAUD            9600
#define PIN_GPS_EN          34               // active low
#define GPS_EN_ACTIVE       LOW
#define PIN_GPS_RST         42               // low for >100 ms resets an L76K
#define PIN_GPS_PPS         41
#define PIN_GPS_STANDBY     40               // high forces the receiver awake

// ---------------------------------------------------------------------------
// Environment — a BME280 on an expansion module, not on the board
// ---------------------------------------------------------------------------
// The same story as the sounder above, and it is worth telling once more
// because the paperwork does not tell it. The expansion kit datasheet on file
// (Rev 1.4, Oct 2025 — roadmap/datasheets) lists a spec table of DA217, L76K,
// the cell and the touch front, and **no environmental sensor at all**. The
// part is one of the modules that plug into the slot, exactly as the sounder
// is, so the board can carry it or not and the datasheet is right either way.
//
// Which means the address is the module's business rather than the board's:
// SDO decides it, both 0x76 and 0x77 are in the wild, and nothing here knows
// which one a given module strapped. So both are tried, first the lower — the
// bare part's own default — and then the other, each held to a chip id and a
// real calibration before it is accepted (Config.h, ENV_ADDR_ALT). That is
// safe on this bus in a way it would not be everywhere: the parts on the V4's
// main I2C are the DA217 and the touch controller at 0x2E, so neither address
// has a neighbour that could answer in the part's place.
//
// It sits on the main I2C bus, with the DA217 accelerometer at 0x26 — and,
// less obviously, with the BQ25896 charger that `Power::battery()` reads. The
// panel is *not* on this bus: it is an ST7789 on SPI and the touch layer is on
// the other I2C host, so an earlier version of this comment claiming a shared
// panel bus was wrong on both counts.
//
// That matters because it changes the safety argument rather than decorating
// it. `Environment::poll()` runs on the loop task and nowhere else, but it is
// not the only reader here: `Power::battery()` reaches the charger from the
// display task too, so this board already has two tasks reading one bus. That
// is the interleave issue 33 describes and has not yet fixed — I2cReg drains a
// read after `requestFrom` has given the lock back — and driving these parts
// adds traffic to it rather than being immune to it.
//
// The exposure is asymmetric and worth knowing: the humidity part checksums
// every reply, so a torn read is refused and counted as a missed interval. The
// BME280 has no checksum on its data registers, so a torn read there is
// published as a reading. Neither is new with this change; both are closed by
// issue 33's per-host lock.
//
// A V4 with no sensor module fitted costs nothing for this: begin() finds
// nothing at either address, says so once, and present() stays false — every
// surface already draws that case, because the T-Beam Supreme has had a real
// one to draw since before this board existed.
#define HAS_ENV             1
#define ENV_KIND            ENV_KIND_BME280
#define ENV_ADDR            0x76
#define ENV_ADDR_ALT        0x77

// And a second one beside it: the expansion's third I2C part is a GXHT3V,
// GXCAS's clone of an SHTC3, at 0x70. The bus scan on the bench unit is what
// named it — `parts=3 acked=0x26 0x70 0x76`, which is the accelerometer, this
// part and the barometer — because 0x70 belongs to the SHTC3 family and to
// nothing else this board could be carrying.
//
// Both parts are driven and both are reported. They measure the same two
// quantities and will not agree to the tenth of a degree, and that is the
// point: which of them a number came from is a thing the operator can see
// rather than a choice made in the firmware (Environment.h).
#define HAS_ENV2            1
