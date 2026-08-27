# HTTP API

Base URL `http://10.42.0.1`. JSON everywhere. Endpoints under `/api/settings`
require HTTP Basic Auth (user `admin`).

## `GET /api/status` (public)
```json
{
  "firmware": "RetiMesh Node", "version": "v0.0.3",
  "ssid": "retimesh-8249CC", "security": "open", "display": true,
  "station": { "configured": true, "ssid": "home", "connected": true, "ip": "192.168.1.42", "rssi": -61 },
  "power": { "profile": "performance", "cpu_mhz": 240, "battery_present": false, "battery_v": 0.1, "battery_pct": 0 },
  "identity": "69dd5082…", "destination": "8836929b…",
  "uptime_s": 1234, "heap_free": 180000, "heap_min_free": 178000, "psram_free": 2000000,
  "diag": { "boot": { "count": 12, "reason": 3, "reason_name": "panic or unhandled exception",
                      "clean": false, "prev_uptime_s": 15132 },
            "heap": { "free": 180000, "min_free": 178000, "largest_block": 110592, "psram_free": 2000000 },
            "stacks": { "loopTask": 3120, "rns": 6284, "radio": 2960, "gps": 1180 },
            "stack_lowest": 1180, "stack_lowest_task": "gps",
            "tables": { "paths": 3, "links": 1, "links_active": 1, "links_pending": 0,
                        "destinations": 2, "announces": 0, "announces_held": 0, "rates": 4 } },
  "radio": { "online": true, "model": "SX1276", "freq_mhz": 868.1, "bw_khz": 125,
             "sf": 8, "cr": 5, "tx_dbm": 7, "sync_word": 18, "preamble": 18,
             "announce_interval": 600, "beacon_interval": 0, "callsign": "retimesh-8249CC",
             "rssi": -68, "snr": 11.5, "rx_packets": 27, "tx_packets": 9, "rx_dropped": 0,
             "announces_tx": 3, "announces_rx": 5, "beacons_tx": 0, "beacons_rx": 2, "apply_error": 0 },
  "peers": { "rns_tcp": 1, "wifi_sta": 1, "tcp_rx_packets": 12 },
  "sd": { "state": "partial", "type": "SDHC", "card_bytes": 15931539456, "volume_bytes": 268435456,
          "used_bytes": 60000000, "last_format": "" },
  "transport": { "enabled": true, "online": true, "lora_mode": "full", "wifi_mode": "access_point",
                 "autointerface": { "enabled": true, "online": true, "address": "fe80::1cdb:d4ff:fe82:49cc", "peers": 1, "group_id": "reticulum" },
                 "interfaces": [ { "name": "LoRa", "mode": "full", "rx_bytes": 1234, "tx_bytes": 567 },
                                 { "name": "WiFi/10.42.0.2", "mode": "access_point", "rx_bytes": 0, "tx_bytes": 0 } ],
                 "path_count": 2,
                 "paths": [ { "hash": "5168bb90…", "hops": 1, "via": "WiFi/10.42.0.2", "age_s": 40 } ] },
  "neighbors": [ { "name": "Anonymous Peer", "version": "", "kind": "announce", "hash": "5168bb90…",
                   "aspect": "lxmf.delivery", "hops": 0, "via": "wifi", "rssi": 0, "snr": 0, "age_s": 40, "count": 3 } ]
}
```
`kind` is `announce`, `station-id` or `beacon`; `via` is `lora` or `wifi`.

`diag` is what a soak run reads off a node it has no console on.

`boot.reason_name` is why the *previous* run ended, taken from the chip's reset
register at startup — `power-on`, `software restart`, `panic or unhandled
exception`, `task watchdog`, `brownout` and so on; `clean` is false for
anything that was not deliberate. `boot.count` is a restart counter in its own
NVS namespace, so resetting the settings does not erase the history. Together
they distinguish a node that has been up all week from one that has quietly
been restarting.

`boot.prev_uptime_s` is how long the run that just ended lasted, updated every
pass of the main loop so it is accurate to well under a second. It is kept in
RTC memory, which survives a panic, a watchdog reset and a software restart but
not a power cut or a brownout — so the field is **absent**, rather than zero,
when the rail dropped. Its absence next to a `brownout` or `panic` is itself
the evidence that the node lost power rather than crashed.

A *present* `prev_uptime_s` of `0` means something different again: the run
ended before the main loop ever ran, i.e. it died during startup. Repeated
zeroes with a rising `boot.count` are a boot loop.

`heap.largest_block` is the biggest single allocation still possible. The gap
between it and `heap.free` is the fragmentation: an allocator reporting 60 KB
free with a 4 KB largest block will fail an 8 KB request while looking healthy
on the free figure alone.

`stacks` gives the bytes of stack each task has never used, and
`stack_lowest_task` names the one closest to its limit — the task that will
trip the stack canary, and which the resulting panic identifies only on a
console nobody is watching. Tasks a build never started (no GNSS receiver, no
SD card) are omitted rather than reported as zero.

`tables` are the Reticulum structures that grow with traffic. A table that
climbs and never falls is where a week-long run runs out of memory, and it is
the part a heap figure alone will not explain.

`airtime` reports channel use and the transmit budget:
`{"short_pct":0.46,"long_pct":0.02,"band":"869.4-869.65 (10 %)",
"band_limit_pct":10,"band_allocated":true,"duty_limit_pct":9.5,
"duty_manual_pct":0,"budget_used":0.002,"locked":false,"retry_after_s":0,
"csma_slot_ms":25,"csma_band":1}`. `band` and `band_limit_pct` are what the
regulator allows for the channel — chosen from the sub-bands the carrier
actually occupies at the configured bandwidth, taking the stricter one when it
straddles a boundary. `band_allocated` is false on the ranges between the
sub-bands, which are not meant for this kind of device and get the strictest
allowance in the plan. `duty_limit_pct` is what the node enforces (the
allowance less a safety margin, or a stricter `duty_manual_pct` when set);
`long_pct` is the share of the last hour spent transmitting and `locked` says
transmissions are being held until `retry_after_s` seconds from now. The CSMA
slot and contention-window band come from the same figures.

`power` reports the profile, CPU clock and the cell:
`{"profile":"performance","cpu_mhz":240,"battery_present":true,
"battery_charging":true,"battery_v":3.53,"battery_pct":10,"pmu":"AXP2101",
"board":"LilyGO T-Beam"}`. `battery_charging` and a trustworthy
`battery_present` need a power-management chip; boards reading an ADC divider
report `false` and infer presence from the voltage.

`gps` appears on boards with a receiver:
`{"enabled":true,"fix":true,"quality":1,"satellites":7,"sentences":1204,
"clock_set":true,"utc":"2026-08-26 21:04:11","position_public":false}`.
Those fields say whether the receiver is working, and `clock_set` says the
node adopted its UTC for the system clock.

The coordinates (`latitude`, `longitude`, `altitude_m`, `hdop`, `speed_kmh`)
are **not** public: this endpoint needs no credentials and the access point may
be open, so they are added only when the caller authenticates as the admin, or
when the operator has set `gps_share_position` to publish them deliberately.
`position_public` says which of those applies. `POST /api/settings/radio
{"gps_enabled":false}` powers the receiver down without a restart.

`storage` reports where the Reticulum store lives: `{"backend":"sd"|"littlefs",
"path":"/sd/rns","lost":false}`. `lost` is true when the card holding the store
was removed — the node keeps routing with what it has in memory but learns no
new paths until it restarts. `sd.reserved` says the store is on the card, which
also blocks formatting it.

## Bulletin board (public)
- `GET /api/board` → `[{"id":1,"author":"…","text":"…"}]` (ordered, no timestamps — no RTC)
- `POST /api/board` `{"author":"…","text":"…"}` → `{"ok":true}`; 50 posts kept

## QR codes (public, except the Wi-Fi one)
`GET /api/qr?what=wifi|portal|address` → `image/svg+xml`, generated on the node.

| `what` | Encodes | Auth |
|---|---|---|
| `wifi` (default) | `WIFI:T:…;S:<ssid>;P:<password>;;` — scan to join the access point | admin, unless the AP is open |
| `portal` | `http://<ip>/` — the LAN address when the node joined a network, else the AP address | none |
| `address` | the node's Reticulum destination hash | none |

The smallest QR version that fits is used, at error-correction level L, with the
standard four-module quiet zone. The display's QR page shows the same Wi-Fi code.

```sh
curl -s "http://10.42.0.1/api/qr?what=portal" -o node.svg
curl -su admin:retimesh "http://10.42.0.1/api/qr?what=wifi" -o join.svg
```

## Settings (auth)
- `GET /api/settings` → `{ radio, wifi, transport, admin }` (password never returned; `has_password`, `default_password` flags)
- `POST /api/settings/radio` `{freq_mhz,bw_khz,sf,cr,tx_dbm,sync_word,preamble,announce_interval,beacon_interval,callsign,duty_cycle_pct,gps_enabled,gps_share_position}` → applied live; `apply_error` in status if the chip rejected it
- `POST /api/settings/wifi` `{ssid,security,password,channel,max_stations,hidden,sta_ssid,sta_password}` → saves, restarts (`"restart":true`); `sta_ssid` blank = station mode off
- `POST /api/settings/transport` `{enabled,lora_mode,wifi_mode,announce_cap,announce_rate_target,announce_rate_grace,announce_rate_penalty,auto_enabled,auto_group_id,power_profile,sd_store}` — the power profile applies live; the other fields restart the node (modes 1 full, 2 gateway, 3 access_point, 4 roaming, 5 boundary; cap in %, rates in s) → saves, restarts
- `GET /api/sd/log` (`?prev=1` for the rotated file) → the SD event log as text
- `GET /api/settings/export` → downloadable JSON of all settings (no identity keys)
- `POST /api/settings/import` (a settings export; sections optional) → applies, restarts
- `POST /api/settings/admin` `{password}`
- `POST /api/settings/reset` → factory defaults, restarts (identity kept)
- `POST /api/settings/sd/format` `{"confirm":"FORMAT"}` → erases the SD card and creates one FAT32 volume; poll `sd.state`/`sd.last_format` in `/api/status` (409 if no card or already formatting)

Errors: `{"error":"<reason>"}` with 400/401/413/500.

## Captive portal
`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, … and any
unknown path redirect to `http://10.42.0.1/`.
