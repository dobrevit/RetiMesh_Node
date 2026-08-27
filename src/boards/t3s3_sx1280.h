#pragma once
// LilyGO T3-S3 carrying the SX1280 (2.4 GHz) module. Same carrier board as the
// sub-GHz T3-S3 — identical SPI, SD, OLED and button wiring — so only the
// radio and its defaults differ. The SX1280 hangs off BUSY and DIO1 exactly as
// the SX1262 does; DIO0 is unused, because there is no SX127x to probe for.
#define BOARD_NAME          "LilyGO T3-S3 (SX1280)"
#define PIN_LORA_SCK        5
#define PIN_LORA_MISO       3
#define PIN_LORA_MOSI       6
#define PIN_LORA_CS         7
#define PIN_LORA_RST        8
#define PIN_LORA_BUSY       34
#define PIN_LORA_DIO1       33
#define PIN_LORA_DIO0       9                // unused on this variant
#define LORA_SPI_BUS        FSPI

// Selects the SX1280 driver instead of probing. See LoRaRadio::begin() for why
// this cannot be auto-detected alongside the sub-GHz parts.
#define RF_MODEM_SX1280     1

// 2.4 GHz defaults. 2445 MHz sits mid-band, clear of Wi-Fi channels 1 and 11
// and of the BLE advertising channels at 2402/2426/2480. 812.5 kHz is the
// SX1280's middle bandwidth and its most common LoRa setting; SF8 keeps a full
// fragment well under a second on the air.
#define RF_FREQ_MHZ         2445.0
#define RF_BW_KHZ           812.5
#define RF_SF               8
#define RF_TX_DBM           10               // SX1280 tops out at 13
#define RF_TCXO_VOLTAGE     0.0              // module has none
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
