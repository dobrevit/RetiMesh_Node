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
// Elecrow ThinkNode M9 — ESP32-S3 with a Semtech LR1110, a 2.4" colour panel,
// a full keyboard on a microcontroller of its own, GNSS and a 2300 mAh cell.
//
// The first board here with a radio that is not a Semtech SX12xx. Everything
// else about it is a variation on ground this firmware has already covered;
// the radio is a new family, and it is the reason this board took the work it
// did (see src/radio/LoRaRadio.cpp, probeLR1110).
//
// Where these numbers come from
// -----------------------------
// Four sources, of which three are firmwares that run on this exact board:
//
//   * Elecrow's own hardware documentation for the M9, which is the only
//     source that names parts and nets rather than pins alone;
//   * the MeshCore variant for this board;
//   * the Meshtastic variant for this board — the most complete of the three,
//     and the only one that drives the keyboard;
//   * a bare-metal third-party port, written from the hardware rather than
//     from either firmware, which independently confirms the keyboard
//     protocol, the panel and the backlight's polarity.
//
// They agree on every pin below. Where they disagree it is over names rather
// than numbers, and each case is called out where it could mislead.
//
// Bench: ESP32-S3 (QFN56) rev 0.2, 16 MB quad flash, 8 MB embedded PSRAM —
// read off the part with esptool. An R8 die is octal, so the env builds
// qio_opi and GPIO 33-37 are spent on the PSRAM; nothing here uses them.

#define BOARD_NAME          "Elecrow ThinkNode M9"

// ---------------------------------------------------------------------------
// The peripheral rail — active low here
// ---------------------------------------------------------------------------
// Elecrow's net name for it is VDD_PERIPH_EN, and it gates the panel and the
// sensor bus. Both firmwares drive it low first thing. Same idea as the
// T-Deck's rail and the opposite polarity, which is why the level is part of
// the board description rather than assumed by the code that raises it.
#define HAS_BOARD_POWER     1
#define PIN_BOARD_POWER     18
#define BOARD_POWER_ACTIVE  LOW

// The native USB pads have to be given up on this board. GPIO 19 and 20 are
// the ESP32-S3's own D-/D+, and this board uses 19 for the panel's tearing
// signal and 20 for the keyboard's I2C data. Left enabled, the USB pad fights
// the keyboard bus. Nothing is lost: the console here is a CH340 bridge on
// UART0, not the chip's own USB.
#define HAS_USB_PAD_CONFLICT 1

// ---------------------------------------------------------------------------
// Radio — LR1110, a new family for this firmware
// ---------------------------------------------------------------------------
#define RF_MODEM_LR1110     1

#define PIN_LORA_SCK        40
#define PIN_LORA_MISO       38
#define PIN_LORA_MOSI       47
#define PIN_LORA_CS         39
#define PIN_LORA_RST        45
#define PIN_LORA_BUSY       41
// The interrupt is this part's DIO9, not a DIO1 — the name here is the
// firmware's generic "primary IRQ line" and the pin is the board's.
#define PIN_LORA_DIO1       42
#define PIN_LORA_DIO0       -1

#define LORA_SPI_BUS        FSPI
#define SPI_BUS_SHARED      1                // panel and card are on it too

// A TCXO on DIO3, at 3.3 V rather than the 1.8 V every SX1262 board here uses.
#define RF_TCXO_VOLTAGE     3.3
// Not an SX126x: DIO2 steers nothing here, the antenna switch is below.
#define RF_DIO2_AS_SWITCH   false

// The antenna switch, and the single most important thing on this page.
//
// The LR1110 drives its own switch from its own DIO lines rather than from the
// MCU's GPIOs, and it does nothing with them until it is told which line means
// what in each mode. Until then the part answers over SPI, reports a firmware
// version, accepts a channel and transmits into a pin that goes nowhere: it is
// online by every measure this firmware has, and it is silent.
//
// This board wires two of them, DIO5 and DIO6; DIO7 and DIO8 are not connected.
// Both reference firmwares carry byte-identical tables, which is as close to
// proof as a pin map gets.
//
// Note the high-frequency transmit row: it is the same as standby, because the
// 2.4 GHz path the LR1110 could otherwise offer is not routed on this board.
// That is why RadioCaps calls this radio sub-GHz — see kLR1110.
#define LR11X0_RF_SWITCH_DIOS \
  { RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC }
#define LR11X0_RF_SWITCH_TABLE {                     \
  /* mode                    DIO5   DIO6 */          \
  { LR11x0::MODE_STBY,     { LOW,   LOW  } },        \
  { LR11x0::MODE_RX,       { HIGH,  LOW  } },        \
  { LR11x0::MODE_TX,       { HIGH,  HIGH } },        \
  { LR11x0::MODE_TX_HP,    { LOW,   HIGH } },        \
  { LR11x0::MODE_TX_HF,    { LOW,   LOW  } },        \
  { LR11x0::MODE_GNSS,     { LOW,   LOW  } },        \
  { LR11x0::MODE_WIFI,     { LOW,   LOW  } },        \
  END_OF_MODE_TABLE,                                 \
}

// No external amplifier: the 22 dBm ceiling is the chip's own high-power PA,
// which RadioLib selects by itself for anything above 14 dBm — and which the
// switch table above has its own row for, so the antenna follows.
#define HAS_PA              0

// A transmit at boot, timed against the interrupt. Worth it here for the same
// reason as on the amplified board: a radio with an unconfigured antenna
// switch reports itself up and says nothing on the air.
#define RADIO_SELFTEST_ON_BOOT 1

// ---------------------------------------------------------------------------
// Display — 240x320 ST7789, sharing the radio's bus
// ---------------------------------------------------------------------------
// A 2.4" 320x240 panel, which is a 240x320 controller mounted landscape. The
// part is an ST7789 and not the ILI9341 the size suggests.
#define HAS_DISPLAY         1
#define DISPLAY_KIND        DISPLAY_KIND_TFT
#define DISPLAY_WIDTH       240
#define DISPLAY_HEIGHT      320
// The glass is mounted landscape and there is no accelerometer to ask, so the
// shell is told where to start. Three quarter turns rather than one: at one
// the picture is the right shape and upside down, which is what the bench
// showed and what the two reference firmwares disagree about between
// themselves.
#define DISPLAY_ROTATION    3

#define PIN_TFT_SCK         PIN_LORA_SCK
#define PIN_TFT_MOSI        PIN_LORA_MOSI
#define PIN_TFT_MISO        PIN_LORA_MISO
#define PIN_TFT_DC          15
#define PIN_TFT_CS          16
#define PIN_TFT_RST         14
#define PIN_TFT_BL          17
#define TFT_SPI_BUS         LORA_SPI_BUS
#define TFT_SPI_HZ          40000000

// An LED behind a transistor on a PWM channel — the ordinary kind, unlike the
// T-Deck's one-wire dimmer — but wired so that it lights when the pin is LOW.
// All three sources agree, one of them by driving it low to turn it on.
#define BACKLIGHT_KIND      BACKLIGHT_KIND_PWM
#define BACKLIGHT_ACTIVE_LOW 1

// No touch layer on this board at all. It is the first colour board here
// without one, so the shell has nothing to point with and the keyboard is the
// only way around it.
#define HAS_TOUCH           0

// ---------------------------------------------------------------------------
// Keyboard — a microcontroller of its own, on its own I2C
// ---------------------------------------------------------------------------
// Not the T-Deck's protocol: this one is register-addressed. Write the key
// register, then read one byte. It also has its own bus, away from the
// sensors, and brings its slave up at 100 kHz.
#define HAS_KEYPAD          1
#define KEYPAD_KIND         KEYPAD_KIND_REG8
#define KEYPAD_ADDR         0x6C
#define KEYPAD_KEY_REG      0x01
#define PIN_KEYPAD_SDA      20
#define PIN_KEYPAD_SCL      21
#define KEYPAD_HZ           100000
// The controller raises this when a key goes down and holds it while it is
// held — idle low, rising edge. One source could not see it move, so the
// keyboard is polled and this is recorded rather than depended on.
#define PIN_KEYPAD_INT      12
#define PIN_KEYPAD_LED      46               // backlight, driven from this chip

#define HAS_TRACKBALL       0

// ---------------------------------------------------------------------------
// The sensor I2C, and the buttons and sounder
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA         7
#define PIN_I2C_SCL         6
#define PIN_OLED_SDA        PIN_I2C_SDA
#define PIN_OLED_SCL        PIN_I2C_SCL
#define PIN_OLED_RST        -1
#define HAS_DISPLAY_VEXT    0                // the rail is PIN_BOARD_POWER, above
#define HAS_BQ25896         0                // an LGS4056 dumb charger, nothing to talk to
#define HAS_DA217           0
#define HAS_PMU             0

// A sounder on a PWM channel, behind a driver transistor. Four sources name
// the pin. Elecrow's tables also show a GPIO 9 in the keyboard's matrix, which
// is a different chip's GPIO 9 and not a conflict.
#define HAS_BUZZER          1
#define PIN_BUZZER          9

#define PIN_BUTTON          0                // BOOT
#define HAS_BUTTON2         0
#define PIN_STATUS_LED      -1

// ---------------------------------------------------------------------------
// microSD — the third device on the shared bus
// ---------------------------------------------------------------------------
#define HAS_SD              1
#define PIN_SD_SCK          PIN_LORA_SCK
#define PIN_SD_MISO         PIN_LORA_MISO
#define PIN_SD_MOSI         PIN_LORA_MOSI
#define PIN_SD_CS           48
#define SD_SPI_BUS          LORA_SPI_BUS

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
// A 2:1 divider on GPIO 13, always connected. Worth knowing: GPIO 13 is
// ADC2 on this chip, and ADC2 is the converter the radio and Wi-Fi stack
// contend for — a reading here can fail while the other one is busy, which is
// a missed sample rather than a wrong one.
#define HAS_BATTERY_ADC     1
#define PIN_BATTERY_ADC     13
#define BATTERY_DIVIDER_RATIO 2.0f

// ---------------------------------------------------------------------------
// GNSS
// ---------------------------------------------------------------------------
// Both reference firmwares name these pins from the receiver's point of view
// in at least one place, so copying either header's names gives a dead UART.
// The direction below is the ESP32's: it listens on GPIO 2.
#define HAS_GPS             1
#define PIN_GPS_RX          2                // ESP32 receives here
#define PIN_GPS_TX          3
#define GPS_BAUD            115200
#define PIN_GPS_EN          11
#define GPS_EN_ACTIVE       LOW
// Asserted high here, which is the opposite of every other receiver this
// firmware drives — so "released" is a low. One published source says so; the
// bench agrees with it, having counted zero sentences from a powered receiver
// while this line was held the other way.
#define PIN_GPS_RST         5
#define GPS_RST_ACTIVE      HIGH
#define PIN_GPS_PPS         4
#define PIN_GPS_STANDBY     10               // high forces the receiver awake
