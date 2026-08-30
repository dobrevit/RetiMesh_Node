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
| Security | open | `wpa2`, `wpa2wpa3`, `wpa3`; the WPA3 modes need an ESP-IDF 5 core, which the current toolchain is — a build on an older core greys them out |
| Password | — | 8–63 chars when secured |
| Channel | 6 | 1–13 |
| Max clients | 8 | 1–10 |
| Hidden SSID | no | |
| Station network / password | off | also join an existing LAN (AP+STA); the AP follows the LAN's channel; AutoInterface and mDNS work on both |

## Reticulum transport (saves and restarts)
| Setting | Default | Notes |
|---|---|---|
| Transport | enabled | disabled = plain bridge (no routing, no announces re-broadcast) |
| LoRa interface mode | `full` | `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| Client interface mode | `full` | one interface per client on :4242 (Sideband, `rnsd`) |
| Peer interface mode | `full` | one interface per zero-config peer — the other nodes and hosts on the Wi-Fi links |
| Power profile | performance | `performance` 240 MHz · `balanced` 160 MHz + Wi-Fi modem sleep · `battery` 80 MHz + Wi-Fi sleep + 20 s display timeout; applied live |
| Zero-config peering (AutoInterface) | enabled | RNS AutoInterface on the access point *and* the station link; group id blank = `reticulum` (peers must share it) |
| Announce cap | 2 % | share of each interface's bandwidth announces may use (rnsd `announce_cap`) |
| Announce rate target / grace / penalty | 0 / 0 / 0 | throttle destinations announcing too often (rnsd `announce_rate_*`); 0 = off |
| Reticulum store on SD | on | where the store belongs when a card is present. **Read-only on this form** — saving the flag alone moved nothing and left the node reading an empty store, so the store is moved with *Use this card* / *Eject* under SD card, which copy the data and restart into the new home. See [Architecture](architecture.md#the-store-has-one-home). |

See [reticulum.md](reticulum.md#interface-modes) for what the modes do.

## Local links (saves; Wi-Fi changes restart)
| Setting | Default | Notes |
|---|---|---|
| Wi-Fi | on | off = no access point and no station; the web server and Reticulum TCP still run on every other link, and the serial console's `WIFI ON` turns it back on |
| USB networking (CDC-NCM) | on | on a board whose own USB is on the connector (T3-S3 family, S3 DevKitC): the composite device's network link, `10.64.<n>.1/24` with DHCP — see [local-link.md](local-link.md). Applies live, no restart. Greyed out with the reason on bridged boards |
| PPP over the serial bridge | off | on the CP2102/CH9102 boards (Heltec V3, Wireless Stick, Wireless Bridge, T-Beam): the node is a PPP *client* on its serial port and the host runs `pppd`; it asks for `10.65.<n>.1` and the host is told to take `.2` — see [local-link.md](local-link.md#ppp-over-the-bridge-uart). Applies live. While a host has PPP open the console on that port is silent. `PPP ON`/`PPP OFF` at the console do the same |
| Serial speed while PPP is on | 115200 | the whole port's speed — console and log included — while the switch above is on; the console's 115200 otherwise. Only the speeds the board is qualified for are offered (`boards.json` `uart.qualification` up to `uart.tested_max_baud`; every board today: 115200); anything else is refused |

A switch the board cannot honour is refused by the API rather than saved.
See [local-link.md](local-link.md).

## Maintenance (saves, applies live)
| Setting | Default | Notes |
|---|---|---|
| Bootloader API | on | `POST /api/system/bootloader` answers; off = flash by hand only |
| …also from the station network | no | by default only a directly attached link (AP, USB, PPP) may ask; the upstream LAN is refused |
| Console over TCP (`maintenance.console_tcp`) | on | the same console on `CONSOLE_TCP_PORT` (4243), reachable over the access point, the station link, `usb0` and `ppp0`. Every caller sends `AUTH <admin password>` first and gets `HELP`, `VERSION` and nothing else until it succeeds; the cable needs no password, because physical access is already more than one. This is the cheap way to configure a node from a distance — the web portal costs 28 616 B of internal RAM on a Heltec Wireless Stick against 272 B for a listener of this shape. Off means the socket does not exist |
| Serial maintenance console | on | the port answers `VERSION`, `STATUS`, `GET`/`SET`, `BOOTLOADER CONFIRM`, …; off = log only. `GET` and `SET` reach every setting in this document by its API name with the section in front (`radio.sf`, `wifi.sta_ssid`) — see [local-link.md](local-link.md#settings-over-the-console) |

The page also carries *Restart node* and *Enter bootloader* — the latter behind
a confirmation and a typed word, and only where the chip can do it.

## Admin
Password 4–32 chars (HTTP Basic Auth, user `admin`). *Factory reset* clears
settings but keeps the identity keys.

Credentials are stored **in the clear**. The admin password, the access-point
password and any station password are plain strings in NVS, and the identity
keys are raw bytes; flash encryption is not enabled, so anyone who can read the
flash can read all of them. The portal is HTTP, so Basic Auth puts the password
on the wire on every request — on the node's own access point that is within
radio range of anyone. Change the default password, and treat a node as
something an attacker with physical access owns completely.

## Backup & provisioning
*Download settings (JSON)* exports radio, Wi-Fi (with password), transport
and admin settings — never the identity keys. *Import & restart* applies such
a file (sections optional) — clone a configuration onto other nodes. The export
contains every password in plain text: treat the file as a credential.

Where the store lives is not imported. That describes the node the backup came
from, not the settings being restored, so it is dropped and the answer says so
rather than failing the whole import.

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
| `SD_POLL_MS`, `SD_LOG_MAX_BYTES` | 3000 / 1 MB | slot polling, event-log rotation |
| `DISPLAY_WIDTH`, `DISPLAY_HEIGHT`, `DISPLAY_COMPACT` | 128 / 64 / 0 | panel size; compact drops pages and columns that do not fit a 64x32 |
| `HAS_DISPLAY_VEXT`, `PIN_DISPLAY_VEXT`, `PIN_OLED_RST` | 0 / — / — | panels on a switched rail (both Heltec boards) |
| `PIN_STATUS_LED` | board | activity LED, `-1` where there is none |
| `HAS_PMU`, `HAS_BATTERY_ADC`, `PIN_BATTERY_ADC` | board | battery sensing; only a PMU can report charging |
| `BATTERY_MIN_V`, `BATTERY_MAX_V` | 3.0 / 4.35 | outside this range means no cell is attached |
| `PMU_VBUS_LIMIT_MA` | 500 | how much the node draws from USB |
| `HAS_GPS`, `PIN_GPS_*`, `GPS_BAUD` | board | u-blox receiver |
| `HAS_PA`, `HAS_RF_SWITCH`, `PIN_RF_RXEN`, `PIN_RF_TXEN` | 0 | external power amplifier and its RF switch |
| `RADIO_SELFTEST_ON_BOOT` | 0 | transmit one frame at boot and time the interrupt — proves the DIO wiring rather than assuming it |
| `DIAG_*` | see `Config.h` | boot counter namespace and diagnostics reporting |
| `ASSET_STAMP` | build hash | set by `tools/asset_stamp.py`; compared at boot against `/assets.json` so a firmware-only update says so |
| `DISPLAY_SLEEP_MS`, `DISPLAY_PAGE_TIMEOUT_MS` | 60000 / 30000 | |
| `RNS_MAX_CLIENTS` | 4 | simultaneous TCP peers |
| `CONSOLE_TCP_PORT`, `MAINT_NET_SESSIONS`, `MAINT_AUTH_MAX_FAILURES`, `MAINT_AUTH_LOCKOUT_MS` | 4243 / 1 / 3 / 30000 | the console over TCP: the port it answers on, how many callers at once, and how many wrong passwords the node takes before it stops listening to guesses for a while |
| `PSRAM_MALLOC_THRESHOLD` | 128 | allocations above this size prefer PSRAM |
| `RING_BYTES` (`TX_RING_BYTES`, `RX_RING_BYTES`, `TCP_IN_RING_BYTES`) | 8192 with PSRAM, 4096 without | the three packet rings between the radio, the TCP clients and the transport. Their storage goes to PSRAM where the board has any and to the internal heap where it does not, which is why the default differs: 24 KB is nothing out of 2 MB of PSRAM and it is a tenth of everything a Heltec Wireless Stick has. One ring holds about sixteen RNS packets at 8192 B and eight at 4096, and a ring too small for its board says so — `LoRa TX ring full` in the log and `lora_rx_drop_ring` in `/api/status`. Override per board in `platformio.ini` |
| `BOARD_USB_NATIVE`, `BOARD_USB_NCM`, `BOARD_USB_BRIDGE`, `BOARD_BRIDGE_AUTO_RESET`, `BOARD_UART_NETWORK`, `BOARD_UART_INSTANCE`, `BOARD_UART_BAUDS`, `BOARD_UART_MAX_BAUD` | from `boards.json` | host connectivity of the PCB, generated by `tools/board_caps.py` — not set by hand. `HAS_PPP` follows from `BOARD_UART_NETWORK` |
| `PPP_RX_RING_BYTES`, `PPP_TX_QUEUE_BYTES`, `PPP_BAUD_DEFAULT` | 4096 / 8192 / 115200 | PPP over the bridge UART: the UART driver's receive ring (drops on overflow), the transmit queue (a frame that will not fit is waited on briefly, then dropped whole), the speed a fresh node stores |
| `ARDUINO_USB_MODE`, `ARDUINO_USB_CDC_ON_BOOT`, `RETIMESH_USB_VID`, `RETIMESH_USB_PID`, `USB_MANUFACTURER`, `USB_PRODUCT`, `USB_NETWORK_INTERFACE`, `USB_PID_IS_TEST_ALLOCATION` | from `boards.json` | which stack owns the chip's USB (0 = OTG, the composite device; 1 = the serial-JTAG unit) and what the composite device calls itself (`_usb_identity`), likewise generated |
| `RESTART_ACK_DELAY_MS`, `RESTART_SETTINGS_DELAY_MS` | 600 / 1500 | how long a restart waits for its acknowledgement to leave |
| `FW_VERSION` | `dev` | set by CI from the tag |
| `CORE_DEBUG_LEVEL`, `RNS_LOG_LEVEL` | 3 / DEBUG compiled | console verbosity (runtime RNS level is INFO) |

## Duty cycle and channel access

The node keeps a rolling record of how long it has transmitted in the last
hour, in one-minute bins, and uses it for two things.

**The duty-cycle limiter.** How much of each hour a node may transmit for is
decided by the sub-band its channel falls in, so the node looks it up rather
than asking. The EU 863–870 MHz SRD plan (ERC 70-03) is built in:

| Sub-band | Allowance | Enforced |
|---|---|---|
| 863–865 MHz | 0.1 % | 0.09 % |
| 865–868 MHz | 1 % | 0.95 % |
| 868–868.6 MHz | 1 % | 0.95 % |
| 868.7–869.2 MHz | 0.1 % | 0.09 % |
| 869.4–869.65 MHz | 10 % | 9.5 % |
| 869.7–870 MHz | 1 % | 0.95 % |

The node holds itself to 95 % of the allowance — airtime is accounted after
each frame, so aiming exactly at the ceiling would cross it. Limits are carried
in hundredths of a percent, which expresses every figure in the plan exactly
apart from the 0.1 % bands, where the margin rounds down to 0.09 %.

**A channel is not a point.** The bandwidth is taken into account: a 125 kHz
carrier centred on 868.6 MHz puts half its energy above that boundary, so it is
held to the stricter of the sub-bands it touches rather than the one its centre
happens to fall in. Move it down to 868.5 and the whole channel fits inside the
1 % sub-band, which is then what applies.

The ranges *between* the sub-bands (868.6–868.7, 869.2–869.4, 869.65–869.7) are
not allocated to this class of device. They are still in the table, carrying
the strictest allowance in the plan, because answering "no band, therefore no
limit" would hand an unlimited budget to exactly the channel that deserves the
least; the log and `/api/status` say `not allocated` when you land on one.

`duty_cycle_pct` on the settings page is an optional stricter cap and never a
looser one; leave it at `0` to follow the band. A channel outside the plan
entirely (another region) has nothing to look up — there the cap becomes your
own limit, and `0` means no limiter at all, which the status page flags.

When the budget is spent the radio stops taking packets off the queue: nothing
is dropped, senders simply see back-pressure until the window slides.
`/api/status` reports the band, the enforced `duty_limit_pct`, `locked` and
`retry_after_s`; the display's radio page shows `duty 0.42/9.5%` or
`duty FULL <n>s`.

At the default SF8/BW125/CR4-5, a full 255-byte frame takes about 0.73 s on
air, so a 1 % budget is worth roughly 49 frames an hour. Announces and beacons
count against it like anything else — if the node is mostly idle this is
invisible, but a busy gateway on a slow spreading factor will notice.

**Channel access (CSMA).** Before transmitting, the node waits for the channel
to fall quiet, holds it quiet for a DIFS (two slots), then counts down a random
contention window, restarting if anything is heard. A slot is 12 symbol times
clamped to 24–100 ms — 25 ms at SF8/BW125. The window is drawn from one of four
bands selected by recent channel use, so as the channel fills, nodes spread
their transmissions further apart instead of all retrying after the same fixed
delay. This matches RNode's behaviour, which matters because RNodes and RetiMesh
nodes share the channel.
