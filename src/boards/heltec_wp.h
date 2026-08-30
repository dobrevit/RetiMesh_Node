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
// Heltec Wireless Paper — ESP32-S3 with an SX1262 and a 2.13" e-ink panel.
//
// The panel is not driven yet, and this header says so by leaving its pins
// out rather than writing down numbers nothing has proved. An e-ink display
// is not an OLED with different dimensions: it is written in whole frames,
// takes the better part of a second to do it, and cannot be repainted on a
// timer the way Display.cpp repaints the panels this firmware already has.
// Bringing it up means abstracting the display behind something that can say
// "this is a slow display, coalesce", so this board starts headless and gets
// its panel when that abstraction exists. Everything else about the board —
// the radio, Wi-Fi, the console, the store — works today.
//
// Pin sourcing, which on this bench is worth stating: the SPI four and Vext
// come from the Arduino core's own variant for this exact board
// (variants/heltec_wireless_paper/pins_arduino.h). The radio's reset, busy
// and IRQ lines are the Heltec S3 family's, where the WiFi LoRa 32 V3's
// variant and the reference RNode_Firmware agree on 12/13/14 — but that
// agreement is about the V3, and no second source for *this* board was to
// hand. RADIO_SELFTEST_ON_BOOT below is what settles it: a wrong line here
// is a radio that fails its self-test on the first boot and says so, rather
// than one that looks up and hears nothing.
#define BOARD_NAME          "Heltec Wireless Paper"

#define PIN_LORA_SCK        9
#define PIN_LORA_MISO       11
#define PIN_LORA_MOSI       10
#define PIN_LORA_CS         8
#define PIN_LORA_RST        12
#define PIN_LORA_BUSY       13
#define PIN_LORA_DIO1       14               // SX1262 IRQ
#define PIN_LORA_DIO0       -1               // no SX127x on this board
#define LORA_SPI_BUS        FSPI

// As on the V3: a TCXO, and DIO2 driving the RF switch. Both are Heltec's
// house wiring for the SX1262 and both are wrong on the T3-S3's module.
#define RF_TCXO_VOLTAGE     1.8
#define RF_DIO2_AS_SWITCH   true

// First board on the bench whose radio pins are inferred rather than sourced
// twice, so prove the interrupt line before trusting a quiet channel.
#define RADIO_SELFTEST_ON_BOOT 1

// Headless until the display is abstracted. HAS_DISPLAY_VEXT stays off with
// it: Vext here is GPIO 45 (the V3's 36 is this board's something else), and
// switching a rail nothing is drawing on only costs a pin.
#define HAS_DISPLAY         0

// No SD slot, no GNSS, no PMU — the same shape as the V3.

#define PIN_BUTTON          0                // PRG, active low, beside the USB socket

// No LED is driven. The Arduino variant for this board carries two answers —
// LED_BUILTIN 35 and LED 18 — and 18 is one of the V3's OLED lines, which is
// the sort of copied-forward number that cost an afternoon on the Wireless
// Stick. Nothing here needs an LED, so nothing here guesses at one.
#define PIN_STATUS_LED      -1

// No SD slot, no GNSS, no PMU. The battery divider is left alone for the same
// reason as the LED: unverified on this board, and nothing reads it yet.
#define HAS_SD              0
#define HAS_GPS             0
#define HAS_PMU             0
#define HAS_BATTERY_ADC     0
