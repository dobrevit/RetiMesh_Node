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
// Heltec Wireless Stick V2 / V2.1 — ESP32 with an SX1276 and a 0.49" 64x32 OLED.
//
// Radio and I2C wiring are the WiFi LoRa 32 V2's, which is what the reference
// RNode_Firmware says of this board (heltec-wireless-stick branch, Boards.h:
// "Same ESP32 and SX1276 pinout as the WiFi LoRa 32 V2"). Two things are not:
//
//   * The panel is 64x32, an eighth of the area every page was written for.
//     DisplayLayout.h turns that into ten columns and four rows, so a page is
//     told what it has rather than having its bottom half clipped away.
//   * The panel is fed from the switched Vext rail rather than permanent 3V3,
//     and Vext is active LOW. Without driving it the panel never powers up and
//     the I2C probe finds nothing — which looks exactly like a broken board.
//
// No SD slot and no GNSS: both compile out through the capability flags.
#define BOARD_NAME          "Heltec Wireless Stick V2"

#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       19
#define PIN_LORA_MOSI       27
#define PIN_LORA_CS         18
#define PIN_LORA_RST        14
#define PIN_LORA_DIO0       26               // SX127x IRQ
#define PIN_LORA_DIO1       35               // unused by this driver on SX127x
#define PIN_LORA_BUSY       -1               // no SX126x here
#define LORA_SPI_BUS        VSPI

// Prove the IRQ line at boot rather than trusting the pin map. The SX1280 work
// showed a wrong IRQ pin reports itself perfectly healthy and simply never
// receives; this board's pins come from a second-hand source, so they get
// checked. Drop this once it has been confirmed on hardware.
#define RADIO_SELFTEST_ON_BOOT 1

#define HAS_DISPLAY         1
#define DISPLAY_WIDTH       64
#define DISPLAY_HEIGHT      32
#define PIN_OLED_SDA        4
#define PIN_OLED_SCL        15
#define PIN_OLED_RST        16
// Active low, and the rail needs a moment to settle before the panel is probed.
// Pin numbers here are the Arduino variant's own (framework-arduinoespressif32,
// variants/heltec_wireless_stick/pins_arduino.h), which is the only source that
// is specifically about this board: Vext 21, RST_OLED 16, SCL 15, SDA 4. The
// reference RNode_Firmware defines a Vext of 36, but that belongs to the
// Heltec32 V4 — a different board, and on this ESP32 pin 36 is input-only, so
// driving it does nothing at all and the panel simply never powers up.
#define HAS_DISPLAY_VEXT    1
#define PIN_DISPLAY_VEXT    21

#define PIN_BUTTON          0                // PRG, active low

// The white LED. Nothing in this firmware drives an LED, and an undriven pin
// on a board that has one wired is left floating — so it is claimed and held
// off here rather than left to pick up whatever is on the rail. If it still
// blinks after this, it is the charging circuit rather than the GPIO: these
// boards blink the charge indicator when no cell is fitted, which is exactly
// the state this one is in.
#define PIN_STATUS_LED      25

#define HAS_SD              0
#define HAS_GPS             0
#define HAS_PMU             0
// The board has a JST LiPo connector and a lithium charging circuit, so a cell
// can be fitted — but nothing here reads its voltage yet, and that is on
// purpose. The reference RNode_Firmware does not read it on this board either,
// so there is no confirmed divider pin to copy, and a guessed one is worse
// here than elsewhere: a wrong IRQ pin fails loudly and obviously, while a
// wrong ADC pin returns a plausible number that is simply not the battery.
//
// Reporting "no battery" is truthful and every caller already handles it.
// Once a cell is on hand the pin can be found the same way the SX1280's were —
// sample the ADC-capable pins and see which one tracks half the pack voltage.
#define HAS_BATTERY_ADC     0
