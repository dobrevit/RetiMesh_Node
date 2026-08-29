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
// Heltec Wireless Bridge — an ESP32-D0WDQ6 with an SX1276, 8 MB of flash and
// 8 MB of PSRAM in an aluminium box with two SMA sockets. No display, no SD
// slot, no GNSS: a headless relay.
//
// The radio wiring is the Wireless Stick's, pin for pin — the Arduino core's
// own variant for this board (variants/heltec_wireless_bridge/pins_arduino.h)
// says SS 18, MOSI 27, MISO 19, SCK 5, RST 14, DIO0 26, DIO1 35, and so does
// boards/heltec_ws.h. What differs is what is missing: no panel, so nothing
// here names one and the display code compiles out; and what is extra: the
// PSRAM, which the build enables with the same flags the T-Beam variant of
// the core uses (BOARD_HAS_PSRAM and the cache workaround this silicon
// revision needs), and which main.cpp then hands the larger allocations to.
#define BOARD_NAME          "Heltec Wireless Bridge"

#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       19
#define PIN_LORA_MOSI       27
#define PIN_LORA_CS         18
#define PIN_LORA_RST        14
#define PIN_LORA_DIO0       26               // SX127x IRQ
#define PIN_LORA_DIO1       35               // unused by this driver on SX127x
#define PIN_LORA_BUSY       -1               // no SX126x here
#define LORA_SPI_BUS        VSPI

// The pins come from the core's variant, not from a board in hand under a
// magnifier; the IRQ line is proved at boot until the board has been seen
// receiving.
#define RADIO_SELFTEST_ON_BOOT 1

#define HAS_DISPLAY         0

#define PIN_BUTTON          0                // KEY, active low, inside the shell

// Three LEDs on the front, named by the core's variant LED/BLE, WIFI and
// LoRa. They are the whole of what a node in a box can show (Leds.h): the
// transport up, a Wi-Fi link ready, the radio online, each flickering as its
// traffic passes.
#define PIN_STATUS_LED      25
#define PIN_WIFI_LED        23
#define PIN_LORA_LED        22

#define HAS_SD              0
#define HAS_GPS             0
#define HAS_PMU             0
// There is a 2-pin lithium connector inside the box, but no known divider pin
// to read it on — the same position as the Wireless Stick, for the same
// reason: a guessed ADC pin returns a plausible number that is not the battery.
#define HAS_BATTERY_ADC     0
