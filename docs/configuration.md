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
| Announce interval | 600 s | 0 = off; the node's own `lxmf.delivery` and `nomadnetwork.node` announces |
| Beacon interval | 0 (off) | RetiMesh quick-probe beacons; 10–3600 s |
| Callsign | (SSID) | printable, ≤ 32 chars; used in announces/beacons |
| Duty-cycled receive | off | SX1262 only, and inert at the default channel — see below |

The page prints the matching `rnsd` `RNodeInterface` block for a peer RNode.

### Duty-cycled receive (`radio.rx_duty_cycle`)

Normally the transceiver listens continuously: on an SX1262 that is a constant
~5 mA floor, paid 24 hours a day whether or not anything is on the air. With
this setting on, the chip sleeps between preamble samples instead. What makes
that safe is the 18-symbol preamble floor every RNode-lineage firmware respects
(`RF_PREAMBLE_SYMS`): the sleep window is sized so the receiver is awake inside
the shortest preamble a conforming sender will ever transmit.

**The window is sized on that floor, not on this node's `preamble` setting.**
Raising `preamble` lengthens what this node *transmits* and does not lengthen
its sleep by a microsecond — what the window has to fit inside is the shortest
preamble *other* nodes send, and no local setting can raise that. (Sizing on
the local setting is how a duty-cycled receiver goes deaf: at `preamble 64` the
window would be 48 symbol times, a standard peer's whole 18-symbol preamble
fits inside it, and the packet is lost with every surface still reporting the
mode healthy.) Lowering `preamble` below 18 *does* shorten the window — the
driver will not expect a preamble longer than the one the radio is configured
for — and at the bottom of the range, 6 symbols, there is nothing left to sleep
through at all, so the mode simply never engages.

Three caveats, all of which the node will tell you about:

- **It ships off.** A transport node's one job is to be listening, so sleeping
  the receiver is opted into per node rather than assumed.
- **SX1262 only.** The SX1276/78 and SX1280 drivers have no such mode, and the
  LR1110 on the ThinkNode M9 ships firmware that cannot drive its interrupt
  lines while asleep. The setting is still accepted on those boards — the chip
  is detected at runtime, so refusing it would break provisioning a mixed fleet
  from one export — and they stay in continuous receive.
- **At the default channel it does nothing.** The receiver must be awake for 8
  symbols at each end of the preamble, leaving `18 − 16 = 2` symbols to sleep
  through — 18 being the floor, whatever this node's own `preamble` is set to —
  and the driver will not sleep for less than its wake-up transition (the 5 ms
  TCXO ramp plus ~1 ms, so 6016 µs). At **SF8/125 kHz a symbol is 2048 µs**,
  giving a 4096 µs sleep — under the threshold, so the receiver stays on. It
  engages from a symbol time of about 3 ms: **SF9 or higher at 125 kHz**, or a
  lower spreading factor at a narrower bandwidth (SF7 at 31.25 kHz and SF8 at
  62.5 kHz both qualify). So the channel is the only dial that moves this: SF
  and bandwidth decide it, the preamble setting does not. There is a ceiling as
  well — the sleep reaches the chip as a 24-bit count of 15.625 µs ticks, about
  262 s — but with a two-symbol window no channel this firmware accepts comes
  near it: the slowest, SF12 at 7.8 kHz, sleeps about 1.05 s. The node still
  checks, because a period over the ceiling is one the driver refuses outright
  rather than falling back to a continuous receive, and a refused arm leaves the
  receiver in standby.

### Checking whether it is actually working on your channel

Two places answer it, and neither is a release note.

`STATUS` on the console reports all of it on one line, computed rather than
echoed. A node with the setting on at the shipped channel:

```
RM STATUS radio rx_duty_cycle=on supported=yes sleep_us=4096 engages=no armed=no
```

...and one on a channel that suits it, actually making the saving:

```
RM STATUS radio rx_duty_cycle=on supported=yes sleep_us=16384 engages=yes armed=yes
```

`engages` says the conditions for the saving are met. `armed` says the receiver
is in the mode right now, and it is what the transceiver's driver accepted
rather than what the other four predict — so it, not `engages`, is the field to
read when what you want to know is whether the saving is being made.

`engages=yes armed=no` is therefore not a normal state: it means the node asked
for a duty-cycled receive on a channel that should have taken one and the driver
refused. The node falls straight back to a continuous receive, so nothing is
lost but the saving — it still hears everything — and the log says why, at
`error` level, with a running count.

The node also says which of the four situations it is in once per settings
apply, and at boot, at `info` level:

```
duty-cycled receive: arming it — the receiver sleeps 16384 us per cycle at SF10/125.0 kHz, sized on a sender preamble of 18 symbols
duty-cycled receive: on, but not on this channel — the 4096 us sleep at SF8/125.0 kHz is not one the driver will take, so the receiver stays on continuously
duty-cycled receive: on, but the SX1276 has no such mode — the receiver stays on continuously
duty-cycled receive: off — the receiver listens continuously
```

`GET /api/settings` carries the same answers as `radio.rx_duty_cycle`,
`radio.caps.rx_duty_cycle_supported`, `radio.rx_duty_cycle_engages` and
`radio.rx_duty_cycle_armed`, plus `radio.rx_duty_cycle_would_engage` — the
engagement question with the switch left out, which is what lets the settings
page say "off, but it would sleep here" instead of blaming the channel. The
page hides the switch entirely on a board whose chip lacks the mode.

## Wi-Fi access point (most rows restart; the marked ones apply live)
| Setting | Default | Notes |
|---|---|---|
| SSID | `retimesh-XXXXXX` (MAC-derived) | custom ≤ 32 chars |
| Security | open | `wpa2`, `wpa2wpa3`, `wpa3`; the WPA3 modes need an ESP-IDF 5 core, which the current toolchain is — a build on an older core greys them out |
| Password | — | 8–63 chars when secured |
| Channel | 6 | 1–13 |
| Max clients | 8 | 1–10 |
| Hidden SSID | no | |
| TX power | 14 dBm | 2–20, one ceiling for the AP and the station together (the chip has one radio). **Applies live, no restart.** The driver rounds down to its own quarter-dBm steps, so the status surfaces read the ceiling back (`wifi_tx_dbm`) rather than echoing the setting. 14 dBm covers a phone at portal range; turn it up for a node genuinely bridging a LAN at range |
| Station network / password | off | also join an existing LAN (AP+STA); the AP follows the LAN's channel; AutoInterface and mDNS work on both |
| Station listen interval | 3 | 1–16 — how many of the LAN's beacon intervals a dozing station may sleep through between wakes. Consulted by the driver only under the battery profile's max modem sleep; higher saves more and answers slower on the maintenance path. **Applies live**, from the next association |
| AP idle auto-off (`wifi.ap_idle_off`) | off | take the access point down once it has stood with no client for the configured minutes. An AP must beacon and cannot sleep, so an empty one is the largest steady draw on a battery node. It comes back on the node's button, on `SET links.wifi_ap on` or `WIFI ON` at the console (serial or TCP :4243 — either wakes even when the switch is already on, but `WIFI ON` writes both Wi-Fi switches, so on a node that keeps its station off it also restarts), or on an admin message; a phone **cannot** wake it, because a down AP sends no beacons to join. The station link, LoRa and the transport keep running throughout. **Applies live** |
| AP idle minutes (`wifi.ap_idle_minutes`) | 10 | 1–1440 — how long the AP must stand empty before it goes down. A client associating (or the AP going down for any other reason) re-arms the clock; a wake re-arms it too, so a woken AP gets its whole window again. **Applies live** |

The TX power, station listen interval and AP idle rows apply live; every
other row restarts the node, because the access point cannot be rebuilt
under the request that changed it. The access point beacons at 400 TU
(~410 ms) rather than the stack's default 100 — a quarter of the beacon
airtime and current, at the price of phones taking a moment longer to list
the network; it is a build-time default (`WIFI_AP_BEACON_TU`), not a
setting. When the idle policy has the AP down, the console's `STATUS` says
`wifi_ap=idle-off`, `/api/status` marks the `wifi_ap` link with
`"idle_down": true`, and the portal's links card says so — all three keep
"down by the idle timer" distinct from "off by the switch", which look the
same from a phone.

## Reticulum transport (saves and restarts)
| Setting | Default | Notes |
|---|---|---|
| Transport | enabled | disabled = plain bridge (no routing, no announces re-broadcast) |
| LoRa interface mode | `full` | `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| Client interface mode | `full` | one interface per client on :4242 (Sideband, `rnsd`) |
| Peer interface mode | `full` | one interface per zero-config peer — the other nodes and hosts on the Wi-Fi links |
| Power profile | performance | `performance` 240 MHz · `balanced` 160 MHz + Wi-Fi min modem sleep (wake every DTIM) · `battery` 80 MHz + Wi-Fi max modem sleep (the station wakes every *listen interval* beacons — see the Wi-Fi table) + 20 s display timeout. On the colour boards the profile also sets how far the idle clock is dimmed — half the configured brightness, a quarter, an eighth — see the display table below. Applied live |
| Node role | not set | What this node is *for*, which is a different question from the power profile: the profile is how hard the node tries, the role is what it is. `unset` · `carried` · `transport`. Today it decides one thing — when the GNSS receiver may stop looking — and later rounds add to it. Applied live |
| Zero-config peering (AutoInterface) | enabled | RNS AutoInterface on the access point *and* the station link; group id blank = `reticulum` (peers must share it) |
| Announce cap | 2 % | share of each interface's bandwidth announces may use (rnsd `announce_cap`) |
| Announce rate target / grace / penalty | 0 / 0 / 0 | throttle destinations announcing too often (rnsd `announce_rate_*`); 0 = off |
| Reticulum store on SD | on | where the store belongs when a card is present. **Read-only on this form** — saving the flag alone moved nothing and left the node reading an empty store, so the store is moved with *Use this card* / *Eject* under SD card, which copy the data and restart into the new home. See [Architecture](architecture.md#the-store-has-one-home). |

See [reticulum.md](reticulum.md#interface-modes) for what the modes do.

### The node role and the GNSS duty cycle (`transport.node_role`)

A GNSS receiver is the most expensive thing on most of these boards that
nobody is using: tens of milliamps, continuously, for a coordinate that on a
node bolted to a mast has not changed since it was bolted there. Until this
setting existed the only control was the on/off switch on the radio page, and
turning the receiver off takes the node's clock with it.

The role is the missing input. It cannot be inferred from the board — the same
firmware on the same board is a handheld on one desk and a relay on the next —
so an operator states it, once, per node:

| Role | Means | The receiver then |
|---|---|---|
| `unset` *(default)* | nobody has said | tracks continuously, exactly as every node did before this setting existed |
| `carried` | a handheld: it moves, and somebody reads its screen | rests **1 minute** at a time once it holds a fix |
| `transport` | a fixed installation: it does not move | rests **5 minutes** at a time once it holds a fix |

Three rules bound all of that, and they are why the feature is safe to leave on:

- **A receiver that has not found itself is never rested.** You cannot
  duty-cycle a search: stopping one halfway does not save the energy, it spends
  it again from the start. A node that cannot see the sky simply keeps looking.
- **A position screen on the glass holds it tracking.** The GNSS page, the sky
  view, the bearing dial and the plot each say so while they are painting, and
  a blanked screen says nothing — so opening one of those ends a rest within a
  screen paint, in any role. The idle clock says nothing either: it is drawn
  *over* whichever page was open, which goes on refreshing underneath it, and a
  page nobody can see is not a page anybody is reading. So a colour board left
  on the GNSS page starts resting when the clock arrives rather than when the
  panel finally blanks, four timeouts later.
- **Every rest is earned again.** A fix has to stand for five seconds *after*
  the receiver was allowed to look again before the next rest is taken, so a
  receiver that stops re-acquiring degrades to continuous tracking rather than
  resting for ever on a fix it held an hour ago.

A resting node reports its last position rather than "no fix" — it was told to
stop looking, and that position is still its best answer — with `resting` true
in `/api/status` and the word on the GNSS page. The age beside it goes on
counting, which is the honest reading.

How the receiver is asked depends on what the board fitted, and only boards
with a receiver are affected at all:

| Board | Receiver | Rested with |
|---|---|---|
| `t-deck` (Plus) | u-blox MIA-M10Q | `UBX-RXM-PMREQ` backup over the UART, woken by a byte on the same line — the only off-switch this board has: no enable line, no standby line, no switched rail. The request also carries the rest's own length as a backstop, so a module that never hears the wake byte comes back by itself rather than needing the board power-cycled. A receiver that stays silent after a wake is prodded again every five seconds — but only where this run has actually asked one to stop, so an `unset` node, or the plain T-Deck whose 43/44 are the case's Grove connector rather than a receiver, drives that port exactly as it did before this setting existed |
| `heltec-v4` | Quectel L76K (expansion kit) | the standby line (GPIO 40), which the firmware already drives high to force the receiver awake |
| `thinknode-m9` | ATGM336H | the standby line (GPIO 10), likewise |
| `tbeam` | u-blox NEO-6M/8M | the power-management chip's GPS rail — a real cut, and the board's own backup supply keeps the almanac so a wake is a warm start |
| every other board | none fitted | nothing to do |

Round 5's solar/unattended preset extends this list of roles rather than
replacing it; the stored values never move.

## Local links (saves; the station switch restarts)
| Setting | Default | Notes |
|---|---|---|
| Wi-Fi access point (`links.wifi_ap`) | on | the node's own network and captive portal. **Applies live, no restart**: the node takes the AP down or up while everything else keeps running, through the same runtime path the AP idle auto-off uses (a short grace lets the reply leave before the AP goes). `SET links.wifi_ap on` at the console asks for it back — and wakes an idled-down AP even when the switch is already on. `WIFI ON` does the same but writes both Wi-Fi switches, so on a node that keeps its station off it also restarts |
| Wi-Fi station (`links.wifi_sta`) | on | joins the configured LAN. Restart-applied: the join is built at start-up. With both Wi-Fi switches off the web server and Reticulum TCP still run on every other link, and the serial console's `WIFI ON` turns both back on |
| USB networking (CDC-NCM) | on | on a board whose own USB is on the connector (T3-S3 family, S3 DevKitC): the composite device's network link, `10.64.<n>.1/24` with DHCP — see [local-link.md](local-link.md). Applies live, no restart. Greyed out with the reason on bridged boards |
| PPP over the serial bridge | off | on the CP2102/CH9102 boards (Heltec V3, Wireless Stick, Wireless Bridge, T-Beam): the node is a PPP *client* on its serial port and the host runs `pppd`; it asks for `10.65.<n>.1` and the host is told to take `.2` — see [local-link.md](local-link.md#ppp-over-the-bridge-uart). Applies live. While a host has PPP open the console on that port is silent. `PPP ON`/`PPP OFF` at the console do the same |
| Serial speed while PPP is on | 115200 | the whole port's speed — console and log included — while the switch above is on; the console's 115200 otherwise. Only the speeds the board is qualified for are offered (`boards.json` `uart.qualification` up to `uart.tested_max_baud`; every board today: 115200); anything else is refused |

A switch the board cannot honour is refused by the API rather than saved.
See [local-link.md](local-link.md).

## Display (saves, applies live)
| Setting | Default | Notes |
|---|---|---|
| Brightness (`display.brightness`) | 80 | 5–100 %; anything below 5 is refused — darkness belongs to the sleep timer, not a setting. Applied live, once per change: the backlight duty on the TFT boards and the panel contrast on the OLED boards, where panel current is close to linear in it, so it is a real power knob there too. E-paper has no brightness to set. Set from the on-glass settings (touch boards) or the console (`SET display.brightness 40`) |

On the colour boards this setting is the *ceiling* rather than the only level.
Those panels rest in two stages before they go dark: the screen being used, lit
at exactly the brightness above; then the idle clock, which is dimmed because
nobody is reading it — to half that brightness under **performance**, a quarter
under **balanced** and an eighth under **battery**, never below the 5 % floor
and never above the setting itself. The stored value is untouched by any of it:
turn the brightness up and every stage moves with it. The idle clock arrives
after the display sleep timeout and the panel blanks entirely at four times it,
which is where the controller is put to sleep as well as the backlight turned
off. Waking from that is a press, a key or a tap away, and the panel itself is
lit about five milliseconds after the wake reaches it — what a finger waits for
is the poll that notices it: every display pass for the case button, and a
quarter of a second for the touch layer and the keyboard, which a dark screen
reads gently rather than ten times a second.

## Maintenance (saves, applies live — except the web portal and mDNS, which restart)
| Setting | Default | Notes |
|---|---|---|
| Bootloader API | on | `POST /api/system/bootloader` answers; off = flash by hand only |
| …also from the station network | no | by default only a directly attached link (AP, USB, PPP) may ask; the upstream LAN is refused |
| mDNS (`maintenance.mdns`) | from the board | answers `<hostname>.local` and advertises the Reticulum port. `6 368 B` of byte-addressable RAM, measured on a Heltec Wireless Stick — the largest single thing a node can decline and still be a node, which is why the boards with least of it start without one. Nothing depends on it; reach the node by address instead. Restart-applied |
| Web portal (`maintenance.web_ui`) | on | off means the routes are never registered and nothing listens on port 80. The largest single thing a small board can decline: `http + dns` bills `22 028 B` of byte-addressable RAM with the portal on against `5 324 B` for the resolver alone, so the portal is about `16 700 B` — measured on a Heltec Wireless Stick that has ~213 KB of it and needs 53 KB for Reticulum. (Older notes quote `28 624 B`; that was the portal, the resolver and mDNS billed on one line.) Administer it over the console instead, on the cable or on TCP. Restart-applied, like Wi-Fi. Refused if it would leave no way in — with the console off, the portal is the only thing answering on a link |
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
| `RF_RX_DUTY_CYCLE` | 0 (off) | default for the duty-cycled receive switch above |
| `RF_TCXO_VOLTAGE` | 1.8 | SX1262 TCXO; 0 for crystal modules |
| `RF_DIO2_AS_SWITCH` | true | SX1262 RF switch on DIO2 |
| `PIN_LORA_*`, `PIN_OLED_*`, `PIN_BUTTON` | T3-S3 map | wiring |
| `AP_SSID_PREFIX` / `AP_SSID` | `retimesh` / — | derived vs fixed SSID |
| `AP_PASSWORD`, `AP_SECURITY_DEFAULT`, `AP_CHANNEL`, `AP_MAX_STATIONS` | open | AP defaults |
| `ADMIN_PASSWORD_DEFAULT` | `retimesh` | |
| `ANNOUNCE_INTERVAL_S`, `BEACON_INTERVAL_S` | 600 / 0 | |
| `HAS_DISPLAY`, `OLED_ADDR`, `OLED_ROTATION` | 1 / 0x3C / 0 | |
| `HAS_SD`, `PIN_SD_*`, `SD_SPI_HZ`, `SD_PARTIAL_PERCENT` | 1 / T3-S3 map / 20 MHz / 50 | microSD slot |
| `SD_LOG_MAX_BYTES` | 1 MB | event-log rotation (how often the slot is polled is not a constant — see [Hardware](hardware.md#microsd-card)) |
| `DISPLAY_WIDTH`, `DISPLAY_HEIGHT`, `DISPLAY_COMPACT` | 128 / 64 / 0 | panel size; compact drops pages and columns that do not fit a 64x32 |
| `HAS_DISPLAY_VEXT`, `PIN_DISPLAY_VEXT`, `PIN_OLED_RST` | 0 / — / — | panels on a switched rail (both Heltec boards) |
| `PIN_STATUS_LED` | board | activity LED, `-1` where there is none |
| `HAS_PMU`, `HAS_BATTERY_ADC`, `PIN_BATTERY_ADC` | board | battery sensing; only a PMU can report charging |
| `BATTERY_MIN_V`, `BATTERY_MAX_V` | 3.0 / 4.35 | outside this range means no cell is attached |
| `PMU_VBUS_LIMIT_MA` | 500 | how much the node draws from USB |
| `HAS_GPS`, `PIN_GPS_*`, `GPS_BAUD` | board | u-blox receiver |
| `HAS_PA`, `HAS_RF_SWITCH`, `PIN_RF_RXEN`, `PIN_RF_TXEN` | 0 | external power amplifier and its RF switch |
| `RADIO_SELFTEST_ON_BOOT` | 0 | transmit one frame and time the interrupt — proves the DIO wiring rather than assuming it. Once per firmware image, not once per boot: the verdict is kept in NVS against the running binary, so a brown-out loop does not re-pay the airtime |
| `DIAG_*` | see `Config.h` | boot counter namespace and diagnostics reporting |
| `ASSET_STAMP` | build hash | set by `tools/asset_stamp.py`; compared at boot against `/assets.json` so a firmware-only update says so |
| `DISPLAY_SLEEP_MS`, `DISPLAY_PAGE_TIMEOUT_MS` | 60000 / 30000 | |
| `RNS_MAX_CLIENTS` | 4 | simultaneous TCP peers |
| `CONSOLE_TCP_PORT`, `MAINT_AUTH_MAX_FAILURES`, `MAINT_AUTH_LOCKOUT_MS` | 4243 / 3 / 30000 | the console over TCP: the port it answers on, and how many wrong passwords the node takes before it stops listening to guesses for a while. One caller at a time, which is fixed rather than tunable — the transport holds a single client slot |
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
