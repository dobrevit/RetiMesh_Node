# HTTP API

Base URL `http://10.42.0.1`. JSON everywhere. Endpoints under `/api/settings`
require HTTP Basic Auth (user `admin`).

## `GET /api/status` (public)
```json
{
  "firmware": "RetiMesh Node", "version": "v0.0.3",
  "ssid": "retimesh-8249CC", "security": "open", "display": true,
  "identity": "69dd5082…", "destination": "8836929b…",
  "uptime_s": 1234, "heap_free": 180000, "heap_min_free": 178000, "psram_free": 2000000,
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

## Bulletin board (public)
- `GET /api/board` → `[{"id":1,"author":"…","text":"…"}]` (ordered, no timestamps — no RTC)
- `POST /api/board` `{"author":"…","text":"…"}` → `{"ok":true}`; 50 posts kept

## Settings (auth)
- `GET /api/settings` → `{ radio, wifi, transport, admin }` (password never returned; `has_password`, `default_password` flags)
- `POST /api/settings/radio` `{freq_mhz,bw_khz,sf,cr,tx_dbm,sync_word,preamble,announce_interval,beacon_interval,callsign}` → applied live; `apply_error` in status if the chip rejected it
- `POST /api/settings/wifi` `{ssid,security,password,channel,max_stations,hidden}` → saves, restarts (`"restart":true`)
- `POST /api/settings/transport` `{enabled,lora_mode,wifi_mode,announce_cap,announce_rate_target,announce_rate_grace,announce_rate_penalty,auto_enabled,auto_group_id}` (modes 1 full, 2 gateway, 3 access_point, 4 roaming, 5 boundary; cap in %, rates in s) → saves, restarts
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
