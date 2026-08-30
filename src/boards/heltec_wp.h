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
// The panel is driven (src/ui/EinkPanel.h). An e-ink display is not an OLED
// with different dimensions: it is written in whole frames, takes the better
// part of a second to do it, and cannot be repainted on the timer the other
// panels use — so it is drawn every pass into memory and shown only when the
// result differs and the panel can afford it. The pins below are the driver
// library's own, and EinkPanel.h holds the two to each other at compile time.
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

// The 2.13" panel, driven since the display grew an interface that can
// express what an update costs (EinkPanel.h). 250x122 landscape; the panel is
// 128x250 in its own orientation and the driver rotates it.
//
// The display pins are the Heltec platform's own, from the driver library's
// board header (heltec-eink-modules, Platforms/WirelessPaper) — which also
// states the LoRa pins above, and states them as 12/13/14, so the three that
// were inferred from the V3 when this board was added now have their second
// source and it agrees with the bench.
#define HAS_DISPLAY         1
#define DISPLAY_KIND        DISPLAY_KIND_EINK
#define DISPLAY_WIDTH       250
#define DISPLAY_HEIGHT      122
#define PIN_EPD_MOSI        2
#define PIN_EPD_SCK         3
#define PIN_EPD_CS          4
#define PIN_EPD_DC          5
#define PIN_EPD_RST         6
#define PIN_EPD_BUSY        7
// Vext feeds every peripheral on this PCB and is active low, as on the other
// Heltec boards. HAS_DISPLAY_VEXT is what the display module keys on.
#define HAS_DISPLAY_VEXT    1
#define PIN_DISPLAY_VEXT    45

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
