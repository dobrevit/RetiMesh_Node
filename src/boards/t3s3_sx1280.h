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
// LilyGO T3-S3 carrying the SX1280 (2.4 GHz) module.
//
// Same carrier board as the sub-GHz T3-S3 for SPI, SD, OLED and the button —
// and NOT the same for the radio, which is the trap. The SX1280 variant hangs
// BUSY on 36 and its IRQ on 9, where the SX1262 uses 34 and 33, and it carries
// an external PA behind an RF switch that has to be steered per direction.
// Pin numbers and the switch follow the reference RNode_Firmware, which
// supports this exact board (Boards.h, BOARD_T3S3 with MODEM == SX1280).
//
// Copying the SX1262 pins here does not fail loudly: the chip answers over SPI
// regardless, so begin() succeeds and the node reports "SX1280 online" while
// the IRQ never fires and the PA stays off in both directions. A radio that
// says it is up and hears nothing is worse than one that admits it is down.
#define BOARD_NAME          "LilyGO T3-S3 (SX1280)"
#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       3
#define PIN_LORA_MOSI       6
#define PIN_LORA_CS         7
#define PIN_LORA_RST        8
#define PIN_LORA_BUSY       36               // SX1280: not the SX1262's 34
#define PIN_LORA_DIO1       9                // SX1280 IRQ: not the SX1262's 33
#define PIN_LORA_DIO0       -1               // no SX127x on this variant
#define LORA_SPI_BUS        FSPI

// Selects the SX1280 driver instead of probing. See LoRaRadio::begin() for why
// this cannot be auto-detected alongside the sub-GHz parts.
#define RF_MODEM_SX1280     1

// The PA sits behind a transmit/receive switch: RXEN high to listen, TXEN high
// to transmit, and never both. RadioLib drives these itself once told which
// pins they are.
#define HAS_RF_SWITCH       1
#define PIN_LORA_RXEN       21
#define PIN_LORA_TXEN       10

// With the PA in circuit the figure that reaches the antenna is not the one
// the chip is asked for. The reference firmware allows up to 20 dBm on this
// board where a bare SX1280 stops at 13, so the ceiling is raised here rather
// than in RadioCaps, which describes chips and not the boards around them.
#define RF_TX_DBM_MAX       20

// 2.4 GHz defaults. 2445 MHz sits mid-band, clear of Wi-Fi channels 1 and 11
// and of the BLE advertising channels at 2402/2426/2480. 812.5 kHz is the
// SX1280's middle bandwidth and its most common LoRa setting; SF8 keeps a full
// fragment well under a second on the air.
#define RF_FREQ_MHZ         2445.0
#define RF_BW_KHZ           812.5
#define RF_SF               8
#define RF_TX_DBM           10
#define RF_TCXO_VOLTAGE     0.0              // SX128x has no TCXO control here
#define RF_DIO2_AS_SWITCH   false

#define HAS_SD              1
#define PIN_SD_MOSI         11
#define PIN_SD_MISO         2
#define PIN_SD_SCK          14
#define PIN_SD_CS           13
#define HAS_DISPLAY         1
#define PIN_OLED_SDA        18
#define PIN_OLED_SCL        17
#define PIN_BUTTON          0                // BOOT
#define HAS_PMU             0
