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
| Station network / password | off | also join an existing LAN (AP+STA); the AP follows the LAN's channel; AutoInterface and mDNS work on both |

## Reticulum transport (saves and restarts)
| Setting | Default | Notes |
|---|---|---|
| Transport | enabled | disabled = plain bridge (no routing, no announces re-broadcast) |
| LoRa interface mode | `full` | `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| Wi-Fi clients mode | `access_point` | applied to every TCP and AutoInterface client interface |
| Power profile | performance | `performance` 240 MHz · `balanced` 160 MHz + Wi-Fi modem sleep · `battery` 80 MHz + Wi-Fi sleep + 20 s display timeout; applied live |
| Zero-config peering (AutoInterface) | enabled | RNS AutoInterface on the AP; group id blank = `reticulum` (peers must share it) |
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
| `PSRAM_MALLOC_THRESHOLD` | 128 | allocations above this size prefer PSRAM |
| `FW_VERSION` | `dev` | set by CI from the tag |
| `CORE_DEBUG_LEVEL`, `RNS_LOG_LEVEL` | 3 / DEBUG compiled | console verbosity (runtime RNS level is INFO) |

## Duty cycle and channel access

The node keeps a rolling record of how long it has transmitted in the last
hour, in one-minute bins, and uses it for two things.

**The duty-cycle limiter.** How much of each hour a node may transmit for is
decided by the sub-band its channel falls in, so the node looks it up rather
than asking. The EU 863–870 MHz SRD plan (ERC 70-03) is built in:

| Sub-band | Allowance |
|---|---|
| 863–865 MHz | 0.1 % |
| 865–868 MHz | 1 % |
| 868–868.6 MHz | 1 % |
| 868.7–869.2 MHz | 0.1 % |
| 869.4–869.65 MHz | 10 % |
| 869.7–870 MHz | 1 % |

The node holds itself to 95 % of the allowance — airtime is accounted after
each frame, so aiming exactly at the ceiling would cross it. `duty_cycle_pct`
on the settings page is an optional stricter cap and never a looser one; leave
it at `0` to follow the band. The frequencies between those rows are not
allocated to this class of device, and a channel outside the plan entirely
(another region) has nothing to look up — there the cap becomes your own limit,
and `0` means no limiter at all, which the status page flags.

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
