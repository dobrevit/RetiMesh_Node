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
// and NOT the same for the radio, which is the trap. The SX1280 hangs BUSY on
// 36 and its IRQ on 9, where the SX1262 uses 34 and 33. Both were confirmed on
// the bench by watching every free GPIO across a transmission.
//
// Copying the SX1262 pins here does not fail loudly: the chip answers over SPI
// regardless, so begin() succeeds and the node reports "SX1280 online" while
// the IRQ never fires. A radio that says it is up and hears nothing is worse
// than one that admits it is down, which is why RADIO_SELFTEST_ON_BOOT is set
// below.
#define BOARD_NAME          "LilyGO T3-S3 (SX1280)"
#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       3
#define PIN_LORA_MOSI       6
#define PIN_LORA_CS         7
#define PIN_LORA_RST        8
#define PIN_LORA_BUSY       36               // confirmed: rises during TX
#define PIN_LORA_DIO1       9                // confirmed: TxDone interrupt
#define PIN_LORA_DIO0       -1               // no SX127x on this variant
#define LORA_SPI_BUS        FSPI

// Selects the SX1280 driver instead of probing. See LoRaRadio::begin() for why
// this cannot be auto-detected alongside the sub-GHz parts.
#define RF_MODEM_SX1280     1

// This is the PLAIN module: no power amplifier and no transmit/receive switch,
// so the chip's own 13 dBm ceiling applies and nothing needs steering. The
// amplified variant is a different board — see t3s3_sx1280_pa.h. Do not add an
// RF switch here on the strength of the reference firmware: its only SX1280
// entry is the amplified one, which is why its build target is named
// firmware-t3s3_sx1280_pa.

// One transmission at boot, timed against the TxDone interrupt. On a board
// whose IRQ pin is guessed wrong the chip still answers over SPI and still
// reports itself online, so nothing in an ordinary boot distinguishes a
// working radio from a deaf one. A transmit that never raises the interrupt
// takes the full 8 s timeout instead of tens of milliseconds, which does.
#define RADIO_SELFTEST_ON_BOOT 1

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
