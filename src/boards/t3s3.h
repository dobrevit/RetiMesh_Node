// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
// ============================================================================
//  boards/t3s3.h — LilyGO T3-S3 v1.2/v1.3 (ESP32-S3)
//
//  ESP32-S3FH4R2: 4 MB flash, 2 MB quad PSRAM, native USB. Ships with either
//  an SX1262 (DIO1/BUSY) or an SX1276/78 (DIO0); LoRaRadio::begin() probes
//  both, so one build serves either. microSD on its own SPI bus, 0.96" OLED,
//  battery through a 100k/100k divider on GPIO 1.
// ============================================================================
#pragma once

#define BOARD_NAME          "LilyGO T3-S3"

#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       3
#define PIN_LORA_MOSI       6
#define PIN_LORA_CS         7
#define PIN_LORA_RST        8
#define PIN_LORA_BUSY       34               // SX1262
#define PIN_LORA_DIO1       33               // SX1262
#define PIN_LORA_DIO0       9                // SX127x

// SPI host for the transceiver. On the ESP32-S3 FSPI is a general-purpose
// bus; the SD card gets HSPI.
#define LORA_SPI_BUS        FSPI

#define HAS_SD              1
#define PIN_SD_MOSI         11
#define PIN_SD_MISO         2
#define PIN_SD_SCK          14
#define PIN_SD_CS           13

#define HAS_DISPLAY         1
#define PIN_OLED_SDA        18
#define PIN_OLED_SCL        17

#define PIN_BUTTON          0                // BOOT

#define HAS_PMU             0                // no power-management chip
#define HAS_GPS             0
#define HAS_BATTERY_ADC     1
#define PIN_BATTERY_ADC     1
