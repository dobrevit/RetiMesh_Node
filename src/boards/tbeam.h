// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  boards/tbeam.h — LilyGO T-Beam v1.1 / v1.2 (ESP32)
//
//  A different machine from the T3-S3 despite the shared radio: classic
//  ESP32 (no native USB, no PSRAM, 4 MB flash), an 18650 holder, a u-blox
//  GPS, and — the part that matters most — a power-management chip that owns
//  the rails. On this board the radio and the GPS are not simply wired to
//  3V3: the PMU has to be told to power them, which is why Pmu::begin() runs
//  before the radio is probed.
//
//  v1.1 carries an AXP192 and an SX1276; v1.2 an AXP2101 and an SX1262. Both
//  PMUs answer at I2C 0x34 and are told apart by their chip id, and the two
//  transceivers are found by the same probe the T3-S3 uses, so one build
//  covers both revisions.
//
//  No microSD slot: the Reticulum store lives in the flash partition.
// ============================================================================
#pragma once

#define BOARD_NAME          "LilyGO T-Beam"

// Shared SPI bus; the transceiver-specific lines are both defined so the
// SX127x and SX1262 probes each find what they need.
#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       19
#define PIN_LORA_MOSI       27
#define PIN_LORA_CS         18
#define PIN_LORA_RST        23
#define PIN_LORA_BUSY       32               // SX1262 (v1.2)
#define PIN_LORA_DIO1       33               // SX1262 (v1.2)
#define PIN_LORA_DIO0       26               // SX127x (v1.1)

#define HAS_SD              0                // no slot on this board

#define HAS_DISPLAY         1
#define PIN_OLED_SDA        21
#define PIN_OLED_SCL        22

#define PIN_BUTTON          38               // the middle "user" button

// The PMU shares the OLED's I2C bus and reports battery voltage itself, so
// there is no ADC divider to read.
#define HAS_PMU             1
#define PIN_PMU_SDA         PIN_OLED_SDA
#define PIN_PMU_SCL         PIN_OLED_SCL
#define PIN_PMU_IRQ         35
#define HAS_BATTERY_ADC     0

// u-blox NEO-6M/NEO-8M on UART1. Present but left unpowered by default —
// nothing uses it yet and it costs tens of milliamps.
#define HAS_GPS             1
#define PIN_GPS_RX          34               // ESP32 receives here
#define PIN_GPS_TX          12
#define GPS_BAUD            9600
