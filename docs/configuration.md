# Configuration

All runtime settings live on **`/settings.html`** (user `admin`, default
password `retimesh`) and in NVS; compile-time defaults are in
[`src/Config.h`](../src/Config.h) and can be overridden with `-D` build flags
per PlatformIO environment.

## Radio (applied live, no reboot)
| Setting | Default | Range / notes |
|---|---|---|
| Frequency | 868.100 MHz | 137–1020 MHz; check your regulations |
| Bandwidth | 125 kHz | 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500 |
| Spreading factor | 8 | 7–12 |
| Coding rate | 4/5 | 4/5–4/8 |
| TX power | 7 dBm | 2–17 dBm (SX127x) / 2–22 dBm (SX1262) |
| Sync word | 0x12 | RNode default; must match peers |
| Preamble | 18 symbols | RNode default |
| Announce interval | 600 s | 0 = off; the node's own `retimesh.node` announce |
| Beacon interval | 0 (off) | RetiMesh quick-probe beacons; 10–3600 s |
| Callsign | (SSID) | printable, ≤ 32 chars; used in announces/beacons |

The page prints the matching `rnsd` `RNodeInterface` block for a peer RNode.

## Wi-Fi access point (saves and restarts)
| Setting | Default | Notes |
|---|---|---|
| SSID | `retimesh-XXXXXX` (MAC-derived) | custom ≤ 32 chars |
| Security | open | `wpa2`; `wpa2wpa3`/`wpa3` need an ESP-IDF 5 core (greyed out) |
| Password | — | 8–63 chars when secured |
| Channel | 6 | 1–13 |
| Max clients | 8 | 1–10 |
| Hidden SSID | no | |

## Reticulum transport (saves and restarts)
| Setting | Default | Notes |
|---|---|---|
| Transport | enabled | disabled = plain bridge (no routing, no announces re-broadcast) |
| LoRa interface mode | `full` | `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| Wi-Fi clients mode | `access_point` | applied to every TCP client interface |
| Announce cap | 2 % | share of each interface's bandwidth announces may use (rnsd `announce_cap`) |
| Announce rate target / grace / penalty | 0 / 0 / 0 | throttle destinations announcing too often (rnsd `announce_rate_*`); 0 = off |

See [reticulum.md](reticulum.md#interface-modes) for what the modes do.

## Admin
Password 4–32 chars (HTTP Basic Auth, user `admin`). *Factory reset* clears
settings but keeps the identity keys.

## Backup & provisioning
*Download settings (JSON)* exports radio, Wi-Fi (with password), transport
and admin settings — never the identity keys. *Import & restart* applies such
a file (sections optional) — clone a configuration onto other nodes.

## Build flags (platformio.ini / `-D`)
| Flag | Default | Purpose |
|---|---|---|
| `RF_FREQ_MHZ`, `RF_BW_KHZ`, `RF_SF`, `RF_CR`, `RF_TX_DBM`, `RF_SYNCWORD`, `RF_PREAMBLE_SYMS` | see above | radio defaults |
| `RF_TCXO_VOLTAGE` | 1.8 | SX1262 TCXO; 0 for crystal modules |
| `RF_DIO2_AS_SWITCH` | true | SX1262 RF switch on DIO2 |
| `PIN_LORA_*`, `PIN_OLED_*`, `PIN_BUTTON` | T3-S3 map | wiring |
| `AP_SSID_PREFIX` / `AP_SSID` | `retimesh` / — | derived vs fixed SSID |
| `AP_PASSWORD`, `AP_SECURITY_DEFAULT`, `AP_CHANNEL`, `AP_MAX_STATIONS` | open | AP defaults |
| `ADMIN_PASSWORD_DEFAULT` | `retimesh` | |
| `ANNOUNCE_INTERVAL_S`, `BEACON_INTERVAL_S` | 600 / 0 | |
| `HAS_DISPLAY`, `OLED_ADDR`, `OLED_ROTATION` | 1 / 0x3C / 0 | |
| `HAS_SD`, `PIN_SD_*`, `SD_SPI_HZ`, `SD_PARTIAL_PERCENT` | 1 / T3-S3 map / 20 MHz / 50 | microSD slot |
| `DISPLAY_SLEEP_MS`, `DISPLAY_PAGE_TIMEOUT_MS` | 60000 / 30000 | |
| `RNS_MAX_CLIENTS` | 4 | simultaneous TCP peers |
| `FW_VERSION` | `dev` | set by CI from the tag |
| `CORE_DEBUG_LEVEL`, `RNS_LOG_LEVEL` | 3 / DEBUG compiled | console verbosity (runtime RNS level is INFO) |
