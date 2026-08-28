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
// Heltec WiFi LoRa 32 V3 — ESP32-S3 with an SX1262 and a 0.96" 128x64 OLED.
//
// Pins come from two sources that agree: the Arduino variant for this exact
// board (variants/heltec_wifi_lora_32_V3/pins_arduino.h) and the reference
// RNode_Firmware's BOARD_HELTEC32_V3. Where they agreed on the Wireless Stick
// they did not, and that cost an afternoon, so the agreement is worth stating.
//
// Note Vext is GPIO 36 here and that is correct — the same number was wrong on
// the Wireless Stick only because that board is an ESP32, where 36 is
// input-only. On an S3 it is an ordinary output.
#define BOARD_NAME          "Heltec WiFi LoRa 32 V3"

#define PIN_LORA_SCK        9
#define PIN_LORA_MISO       11
#define PIN_LORA_MOSI       10
#define PIN_LORA_CS         8
#define PIN_LORA_RST        12
#define PIN_LORA_BUSY       13
#define PIN_LORA_DIO1       14               // SX1262 IRQ
#define PIN_LORA_DIO0       -1               // no SX127x on this board
#define LORA_SPI_BUS        FSPI

// The SX1262 here has a TCXO and uses DIO2 to drive its RF switch. Both differ
// from the T3-S3's SX1262 module, and getting either wrong leaves a radio that
// initialises and then hears nothing.
#define RF_TCXO_VOLTAGE     1.8
#define RF_DIO2_AS_SWITCH   true

// Prove the interrupt line on the first boot. Two sources agreeing is better
// than one, but neither is this board on this bench.
#define RADIO_SELFTEST_ON_BOOT 1

#define HAS_DISPLAY         1
#define DISPLAY_WIDTH       128
#define DISPLAY_HEIGHT      64
#define PIN_OLED_SDA        17
#define PIN_OLED_SCL        18
#define PIN_OLED_RST        21
// The panel sits on the switched Vext rail, active low, as the Wireless Stick's
// does. It needs the rail up and its reset released before it will answer.
#define HAS_DISPLAY_VEXT    1
#define PIN_DISPLAY_VEXT    36

#define PIN_BUTTON          0                // PRG
#define PIN_STATUS_LED      35

#define HAS_SD              0
#define HAS_GPS             0
#define HAS_PMU             0

// The board reads its cell on GPIO 1, but only while GPIO 37 holds the divider
// enabled — and nothing here drives that yet. A divider read without its
// control line returns a plausible number that is not the battery, which is a
// worse failure than reporting no battery at all. Left off until it can be
// checked against a cell, as on the Wireless Stick.
#define HAS_BATTERY_ADC     0
