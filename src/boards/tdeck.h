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
// LilyGO T-Deck — ESP32-S3 with an SX1262, a 2.8" colour panel, a capacitive
// touch layer, a physical keyboard on its own microcontroller and a trackball.
//
// The first board here that is a terminal rather than a gateway: it has the
// keys to type a message on. Everything the LVGL shell was built for on the
// Heltec V4 applies, with a real keyboard where that board had only glass.
//
// Where these numbers come from
// -----------------------------
// Two independent sources agreeing on every pin, which is the standard this
// project prefers and rarely gets:
//
//   * LilyGO's own `examples/UnitTest/utilities.h` in the T-Deck repository —
//     the vendor's pin map;
//   * the Meshtastic variant for this board (`variants/esp32s3/t-deck`), an
//     unrelated firmware that has run on it for years.
//
// They agree on the SPI bus, the radio, the panel, the touch layer, the
// trackball, the card and the battery pin. Where they differ it is in naming
// rather than numbers, and both cases are noted below.
//
// Bench: ESP32-S3 (QFN56) rev 0.2, 16 MB quad flash, 8 MB embedded PSRAM —
// read off the part with esptool, not taken from a datasheet, because the
// PSRAM width decides the memory type the env must build with and an R8 part
// is octal. That also spends GPIO 33-37, none of which this board uses.

#define BOARD_NAME          "LilyGO T-Deck"

// ---------------------------------------------------------------------------
// The peripheral rail — the first thing this board needs and the only one of
// its kind here
// ---------------------------------------------------------------------------
// Nothing on this board answers until GPIO 10 is high: the radio, the card,
// the panel, the keyboard and both I2C residents sit behind it. A probe run
// before this is raised finds an empty bus and a missing transceiver, and
// reports a wiring fault for what is an unpowered board.
//
// It is not a PMU — there is no chip to talk to, just a load switch — so it
// cannot go through Pmu.h. main.cpp raises it before anything touches a bus.
// LilyGO calls it BOARD_POWERON and Meshtastic KB_POWERON; the schematic has
// it gating far more than the keyboard, and both firmwares raise it first.
#define HAS_BOARD_POWER     1
#define PIN_BOARD_POWER     10
#define BOARD_POWER_ACTIVE  HIGH
// Half a second, which is the vendor's own number and five times the default.
// This one line was three separate bugs.
//
// Everything on this board is behind this rail — the panel as well as the
// keyboard and the touch controller — and a tenth of a second is not enough
// for any of them. The keyboard is a microcontroller that has to boot, and it
// shares an I2C bus with the touch controller, so probing early can leave
// neither answering; that read as two dead devices rather than one impatient
// probe. And the panel, configured before it was ready, took only part of its
// initialisation: the bench saw wrong colours and glyphs a pixel wide with
// most of their dots missing, which looks like a driver that does not
// understand the controller and is not.
//
// Worth remembering the shape of it: three symptoms in three subsystems, one
// cause, and the two "fixes" aimed at the panel itself — a software reset, and
// then the full ST7789 power and gamma block to go with it — both made it
// worse, because neither was what was wrong.
#define BOARD_POWER_SETTLE_MS 500

// ---------------------------------------------------------------------------
// Radio — SX1262 on the shared SPI bus
// ---------------------------------------------------------------------------
#define PIN_LORA_SCK        40
#define PIN_LORA_MISO       38
#define PIN_LORA_MOSI       41
#define PIN_LORA_CS         9
#define PIN_LORA_RST        17
#define PIN_LORA_BUSY       13
#define PIN_LORA_DIO1       45               // SX1262 IRQ
#define PIN_LORA_DIO0       -1               // no SX127x here

// FSPI (SPI2), and the panel and the card are on it too — see the note under
// the display. Every driver here brackets its transfers with
// beginTransaction/endTransaction, which take the Arduino core's per-bus
// mutex, and every one drives its own chip select by hand.
#define LORA_SPI_BUS        FSPI
#define SPI_BUS_SHARED      1                // panel and card are on it too

// Both sources agree: a TCXO at 1.8 V and DIO2 steering the antenna switch.
// These are the firmware's defaults as well, and are repeated here because a
// board header that omits them reads as "not known" rather than "the same".
#define RF_TCXO_VOLTAGE     1.8
#define RF_DIO2_AS_SWITCH   true

// A bare switch and no amplifier: what the chip is driven at is what leaves
// the antenna, less the usual losses.
#define HAS_PA              0

// ---------------------------------------------------------------------------
// Display — 240x320 ST7789, sharing the radio's bus
// ---------------------------------------------------------------------------
// The glass is landscape 320x240 and the controller is portrait 240x320; the
// shell turns it in the controller's own MADCTL, which costs nothing per
// pixel. DISPLAY_WIDTH/HEIGHT are the controller's, as TftPanel expects.
#define HAS_DISPLAY         1
#define DISPLAY_KIND        DISPLAY_KIND_TFT
#define DISPLAY_WIDTH       240
#define DISPLAY_HEIGHT      320
// The glass is mounted landscape and there is no accelerometer to work that
// out, so the shell is told where to start. One quarter turn puts the long
// axis across the keyboard, which is the way the case is held.
#define DISPLAY_ROTATION    1
// Inverted, like the other colour boards here — both published configurations
// for this panel say so. Stated rather than inherited because the bench once
// argued otherwise: the first look showed the dark design as white and yellow,
// which reads as an inversion fault. It was not one. The panel was being
// configured before the rail behind it had settled and was taking only part of
// its initialisation; with the rail's half second above, this value is right.
#define DISPLAY_INVERT      1

// One bus for the panel, the radio and the card. Every other board here gives
// the panel a bus of its own precisely so the radio never waits behind a
// blit — this board does not offer the choice: all three chip selects hang off
// one set of wires, and the ESP32-S3's GPIO matrix cannot let two peripherals
// drive one output pin. So they share, and the sharing is safe rather than
// merely tolerated:
//
//   * SPIClass objects constructed on the same bus number resolve to the same
//     underlying bus struct, whose mutex beginTransaction takes and
//     endTransaction releases — so the radio task and the display task
//     serialise against each other in the core, not by convention;
//   * no driver here attaches a hardware chip select (nothing calls setHwCs),
//     so the `ss` argument to SPIClass::begin is inert and each driver's
//     digitalWrite is the only thing that moves a CS line.
//
// What sharing does cost is latency: a full-frame blit holds the bus for a
// few milliseconds, and a packet that arrives during one waits. The radio's
// receive is interrupt-driven into a task that reads the chip afterwards, so
// waiting is all it does — nothing is dropped.
#define PIN_TFT_SCK         PIN_LORA_SCK
#define PIN_TFT_MOSI        PIN_LORA_MOSI
#define PIN_TFT_MISO        PIN_LORA_MISO
#define PIN_TFT_DC          11
#define PIN_TFT_CS          12
// No reset line: the panel's is tied to the board's own, so it comes out of
// reset with the MCU and there is nothing to pulse.
#define PIN_TFT_RST         -1
#define PIN_TFT_BL          42
#define TFT_SPI_BUS         LORA_SPI_BUS
#define TFT_SPI_HZ          40000000

// The backlight is not an LED on a PWM pin. GPIO 42 drives an AW9364 one-wire
// dimmer, whose brightness is a counter inside the part: holding the line high
// turns it on at full, and each further low-high pulse steps it down one of
// sixteen levels, wrapping round at the bottom. Driving it with a PWM channel —
// which is what the other colour board here does, and what one of the two
// reference firmwares for this board does — sends the part twenty thousand
// step pulses a second and lands on whatever level the wrap happens to leave.
// It lights, which is why it passes for working; it is not a dimmer.
//
// So this board asks for the part's own protocol instead. See TftPanel.cpp.
#define BACKLIGHT_KIND      BACKLIGHT_KIND_AW9364

// ---------------------------------------------------------------------------
// Touch — GT911, on the board's one I2C bus
// ---------------------------------------------------------------------------
// Unlike the Heltec V4's touch layer this one has no bus to itself: the
// controller and the keyboard share SDA 18 / SCL 8. TouchInput sees that the
// pins match the main bus and uses Wire rather than standing up a second one.
#define HAS_TOUCH           1
#define TOUCH_KIND          TOUCH_KIND_GT911
#define PIN_TOUCH_SDA       18
#define PIN_TOUCH_SCL       8
#define PIN_TOUCH_RST       -1               // tied to the board reset
#define PIN_TOUCH_INT       16
#define TOUCH_ADDR          0x5D             // 0x14 on some panels; begin() probes both
// This controller is configured for the way the glass is mounted and already
// answers in the frame the shell draws in, so its points are passed through
// untouched. Measured: a tap at the top-left corner came back as 11,7 — small
// on both axes, which it could only be if the controller and the picture agree
// about which corner that is. Turned to match the panel's rotation instead, the
// same tap landed at the bottom-left, and every press missed what it was aimed
// at while the layer itself was working perfectly.
#define TOUCH_PRE_ROTATED   1
// ...and counts Y from the other edge. Measured: with the focused control
// sitting at y 192-237, a tap on it reported y 26, and 239 - 26 is 213 — inside
// it. The x it reported was already right. A mirrored axis is why the glass
// looked completely dead while working perfectly: every press landed at the
// reflection of the finger, so the only thing that ever answered a tap was
// waking the idle clock, which does not care where the tap was.
#define TOUCH_MIRROR_Y      1

// ---------------------------------------------------------------------------
// Keyboard — an ESP32-C3 of its own, answering on I2C
// ---------------------------------------------------------------------------
// The keyboard is not a matrix this firmware scans: a second microcontroller
// scans it and hands over one ASCII byte per keypress on request. See
// src/ui/Keypad.h for what that protocol is and is not.
#define HAS_KEYPAD          1
#define KEYPAD_ADDR         0x55
#define PIN_KEYPAD_SDA      18
#define PIN_KEYPAD_SCL      8
// LilyGO calls GPIO 46 the keyboard interrupt and Meshtastic the keyboard
// backlight — the one place the two sources disagree. Neither reading is
// worth depending on: the keyboard is polled, as the touch layer is, so the
// line is left alone until somebody with a scope can say which it is.
#define PIN_KEYPAD_INT      -1

// ---------------------------------------------------------------------------
// Trackball — four direction lines and a press
// ---------------------------------------------------------------------------
// Active low, one falling edge per detent. The press is the BOOT pin, which
// is also this board's only button, so PIN_BUTTON below is the same pin: a
// click of the ball is a press of the button, and the page stack and the
// shell both already know what to do with that.
#define HAS_TRACKBALL       1
#define PIN_TRACKBALL_UP    3
#define PIN_TRACKBALL_DOWN  15
#define PIN_TRACKBALL_LEFT  1
#define PIN_TRACKBALL_RIGHT 2

#define PIN_BUTTON          0                // BOOT, and the trackball's click
#define HAS_BUTTON2         0
#define PIN_STATUS_LED      -1
#define HAS_BUZZER          0                // a speaker, not a sounder — not driven here

// ---------------------------------------------------------------------------
// The general-purpose I2C bus, and what is on it
// ---------------------------------------------------------------------------
#define PIN_I2C_SDA         18
#define PIN_I2C_SCL         8
// 100 kHz, not the 400 this firmware uses everywhere else. The keyboard's
// microcontroller brings up its I2C slave at 100 kHz, and it shares this bus
// with the touch controller — so the slowest part on the wire sets the rate
// for all of it. The touch controller is happy either way, and the vendor's
// own firmware clocks this bus at 100 kHz for the same reason.
#define I2C_HZ              100000
// No OLED, but the display module's I2C names still have to resolve.
#define PIN_OLED_SDA        PIN_I2C_SDA
#define PIN_OLED_SCL        PIN_I2C_SCL
#define PIN_OLED_RST        -1
#define HAS_DISPLAY_VEXT    0                // the rail is PIN_BOARD_POWER, raised in setup()
#define HAS_BQ25896         0
#define HAS_IMU             0
#define HAS_PMU             0

// ---------------------------------------------------------------------------
// microSD — the third device on the shared bus
// ---------------------------------------------------------------------------
#define HAS_SD              1
#define PIN_SD_SCK          PIN_LORA_SCK
#define PIN_SD_MISO         PIN_LORA_MISO
#define PIN_SD_MOSI         PIN_LORA_MOSI
#define PIN_SD_CS           39
#define SD_SPI_BUS          LORA_SPI_BUS

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
// A plain 100k/100k divider on GPIO 4, always connected — no enable line to
// raise, unlike the Heltec V4's. The ratio here is the divider's own; the
// Meshtastic variant carries 2.11 instead, which is that ratio with an
// empirical trim for their reader. Ours is a different reader, so it starts
// from the circuit and is corrected against a meter if the bench disagrees.
#define HAS_BATTERY_ADC     1
#define PIN_BATTERY_ADC     4
#define BATTERY_DIVIDER_RATIO 2.0f

// ---------------------------------------------------------------------------
// GNSS — the Plus only
// ---------------------------------------------------------------------------
// The Plus fits a receiver on GPIO 43/44; the plain T-Deck runs those pins out
// to the Grove connector and fits nothing. Both are this one env, and it
// assumes the receiver is there — the same call this project already made for
// the Heltec V4's expansion kit, and for the same reason: the bench unit is a
// Plus, and a board without one loses nothing but a UART nobody is talking on.
// A plain T-Deck reports a receiver that never sends a sentence, which the GPS
// page states plainly rather than hiding.
//
// Worth knowing on a Plus: the Grove connector is still on the case but its
// pins are the ones the receiver took, so it is not usable as an expansion
// port on this variant.
#define HAS_GPS             1
// Which pin is which is the one place a reference firmware here gets it
// backwards: one of the three names these from the receiver's point of view,
// so copying its header gives a dead UART. The vendor's own call settles it —
// HardwareSerial::begin(baud, config, rx, tx) is passed 44 then 43 — so 44 is
// where the ESP32 listens.
#define PIN_GPS_RX          44               // ESP32 receives here
#define PIN_GPS_TX          43
// The u-blox MIA-M10Q's own default, and the rate this board is verified at:
// a fix, twelve satellites and a UTC clock. LilyGO's page for the Plus says
// 9600, which produced nothing here — though that page still describes the
// earlier Quectel this slot used to carry, so it may simply belong to a
// different part.
//
// Left with an unexplained gap, which is worth recording rather than papering
// over: this same rate also read zero sentences for a while before it started
// working, with no firmware change in between. Gps::parse counts every
// checksum-valid sentence whether or not there is a fix, so a receiver merely
// searching would still count — a zero means nothing parseable arrived at all,
// and being indoors does not on its own account for it. Whatever held it quiet
// cleared on its own across a power cycle.
//
// So if a unit ever reads zero here: the number below is not the first thing
// to doubt. Power-cycle it and give it a minute before touching anything. The
// vendor's own firmware sweeps 9600, 38400 and 115200 rather than trusting any
// single figure, which is the fallback if a board turns up with the other
// receiver in it.
#define GPS_BAUD            38400
