# HTTP API

Base URL `http://10.42.0.1`. JSON everywhere. Endpoints under `/api/settings`
require HTTP Basic Auth (user `admin`).

## Radio region and capabilities

`GET /api/settings` carries `radio.region` and a `radio.caps` object describing
what the transceiver in this node can be asked for. `POST /api/settings/radio`
accepts `region` alongside the channel fields and validates the channel against
both the region and the chip.

```json
"radio": {
  "region": "eu868", "model": "SX1276", "tx_dbm_max": 17,
  "caps": {
    "model": "SX1276", "freq_min_mhz": 137, "freq_max_mhz": 1020,
    "bandwidths_khz": [7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500],
    "sf_min": 7, "sf_max": 12, "tx_min_dbm": 2, "tx_max_dbm": 17,
    "regime": "EU 863-870 SRD", "max_dwell_ms": 0,
    "regions": [
      { "key": "eu868", "name": "Europe 863-870 MHz", "low_mhz": 863, "high_mhz": 870,
        "regime": "EU 863-870 SRD", "dwell_ms": 0,
        "default_mhz": 869.525, "default_bw_khz": 125, "default_sf": 8 }
    ]
  }
}
```

`region` is a stored setting, not something inferred from the frequency. The
inference runs the wrong way round: the band a node may use is a fact about
where it is standing, and the channel is chosen inside it — 868.1 MHz is a
legal channel in Europe and an illegal one in the US. Keys are `eu868`,
`us915`, `ism2400` and `custom`. A node configured before the field existed is
migrated from its frequency at load, so one already obeying an EU duty cycle
keeps obeying it rather than landing in `custom` where nothing is enforced.

`caps.regions` lists **only the regions this transceiver can reach** — an
SX1280 node does not offer `eu868`, because choosing it could not be honoured.
`custom` is always listed, and is the one entry whose `low_mhz`/`high_mhz` and
`default_mhz` are taken from the chip's own tuning range rather than from a
band plan.

Each region says what governs there. `dwell_ms` is non-zero only for `us915`,
where FCC 15.247 caps how long one transmission may hold a channel rather than
how much of an hour may be used; the node checks its longest frame against that
and warns at boot when it does not fit. `bandwidths_khz` are matched to within
0.001 kHz, the same tolerance the driver uses, so a value the API accepts is
one the radio will take.

A rejected channel says which bound it missed and names the transceiver, e.g.
`frequency must be 2400-2483.5 MHz in 2.4 GHz ISM on the SX1280` — the bound is
the region's allocation intersected with what the chip can tune, which is why it
stops at the top of the ISM band rather than at the SX1280's own 2500 MHz.

`GET /api/export` includes `region`, and `POST /api/import` validates the radio
section against the same region and capability bounds as the POST above — so an
export always restores onto the node it came from, and never carries a channel
past the checks on the way in.

## `GET /api/status` (public)
```json
{
  "firmware": "RetiMesh Node", "version": "v0.0.3", "board": "LilyGO T3-S3",
  "ssid": "retimesh-8249CC", "hostname": "retimesh-8249cc", "security": "open", "display": true,
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
             "rx_dropped_ring": 0, "rx_dropped_reassembly": 0, "rx_dropped_partial": 0,
             "rx_crc_errors": 4, "rx_bad_length": 0,
             "announces_tx": 3, "announces_rx": 5, "beacons_tx": 0, "beacons_rx": 2, "apply_error": 0 },
  "peers": { "rns_tcp": 1, "wifi_sta": 1, "tcp_rx_packets": 12 },
  "wifi_enabled": true,
  "local_links": [
    { "name": "wifi-ap",  "type": "wifi_ap",  "hardware": true, "firmware": true, "enabled": true,
      "phase": "ready", "up": true, "ip": "10.42.0.1", "addressing": "static", "uptime_s": 1234, "clients": 1 },
    { "name": "wifi-sta", "type": "wifi_sta", "hardware": true, "firmware": true, "enabled": true,
      "phase": "ready", "up": true, "ip": "192.168.1.42", "addressing": "dhcp", "uptime_s": 1200 },
    { "name": "usb0",     "type": "usb_ncm",  "hardware": true, "firmware": true, "enabled": true,
      "phase": "ready", "up": true, "ip": "10.64.84.1", "addressing": "static", "uptime_s": 1230, "clients": 1 },
    { "name": "ppp0", "type": "ppp_uart", "hardware": false, "firmware": false, "enabled": false, "phase": "disabled",
      "reason": "this board has no bridge UART to carry PPP" }
  ],
  "sd": { "state": "partial", "type": "SDHC", "card_bytes": 15931539456, "volume_bytes": 268435456,
          "used_bytes": 60000000, "last_format": "" },
  "transport": { "enabled": true, "online": true, "lora_mode": "full", "wifi_mode": "full", "auto_mode": "full",
                 "autointerface": { "enabled": true, "online": true, "address": "fe80::1cdb:d4ff:fe82:49cc", "peers": 1, "group_id": "reticulum",
                                    "peer_list": [ { "address": "fe80::2cdb:d4ff:fe82:1f20", "age_s": 3, "datagrams": 118 } ] },
                 "interfaces": [ { "name": "LoRa", "mode": "full", "rx_bytes": 1234, "tx_bytes": 567 },
                                 { "name": "WiFi/10.42.0.2:51022", "mode": "full", "rx_bytes": 0, "tx_bytes": 0 },
                                 { "name": "Auto/fe80::2cdb:d4ff:fe82:1f20", "mode": "full", "rx_bytes": 9012, "tx_bytes": 3400 } ],
                 "path_count": 2,
                 "paths": [ { "hash": "5168bb90…", "hops": 1, "via": "WiFi/10.42.0.2:51022", "age_s": 40 } ] },
  "neighbors": [ { "name": "Anonymous Peer", "version": "", "kind": "announce", "hash": "5168bb90…",
                   "aspect": "lxmf.delivery", "hops": 0, "via": "wifi", "rssi": 0, "snr": 0, "age_s": 40, "count": 3 } ]
}
```
`kind` is `announce`, `station-id` or `beacon`; `via` is `lora` or `wifi`.

`hostname` is the mDNS name this node answers to, so it is reachable at
`<hostname>.local` on any network with multicast DNS. It is derived from the
access-point name rather than fixed, because every node answering to the same
`retimesh.local` meant the second one on a network was renamed unpredictably by
conflict resolution and neither could be addressed by a name you could guess.
Renaming the access point renames the node.

Both advertised services carry TXT records — `name`, `node` (the Reticulum
destination), `fw` and `board` — so browsing `_rns._tcp` or `_http._tcp` lists
every node on the network with enough to tell them apart without opening each
one:

    avahi-browse -rt _rns._tcp

`rx_dropped` is the sum of the three counters below that lost a packet the
node had already accepted, and the breakdown says which:

- `rx_dropped_ring` — the RX ring was full, so the radio dropped rather than
  stalling. This is the Reticulum task failing to keep up, not a radio problem.
- `rx_dropped_reassembly` — the second fragment of a split packet did not fit
  the reassembly buffer.
- `rx_dropped_partial` — a half-assembled packet was abandoned because a
  fragment with a different sequence arrived first. Two senders interleaving
  fragments destroy each other's packets this way.
- `rx_crc_errors` — the frame failed its CRC, or the interrupt was spurious.
  Nothing was ever decoded. Hundreds an hour means interference, which looks
  nothing like a slow consumer and used to be indistinguishable from one.
- `rx_bad_length` — the frame was shorter than a header or longer than the
  maximum, and was discarded before decoding.
- `rx_spurious_irq` — the radio raised its interrupt with no completed
  reception to collect. TxDone and channel-activity results share that line, so
  a small number is normal. A large and growing one means receptions are not
  being acknowledged to the chip and the same packet is being presented over
  and over, which shows up as `rx_dropped_ring` climbing on an idle channel.

The distinction matters because the fixes have nothing in common: a full ring
is a software scheduling problem, a CRC storm is an RF one, and interleaved
fragments are a channel-contention one.

These counters do **not** total everything heard on their own, and two of them
count a different unit from the rest. To reconstruct the whole picture:

- `rx_crc_errors` and `rx_bad_length` count **frames** thrown away before
  anything was decoded.
- Every frame that does decode is either one fragment of a reassembly still in
  progress, or it completes a packet.
- A completed packet lands in exactly one of `rx_packets`, `beacons_rx` (when
  it turns out to be a RetiMesh beacon or an RNode station ID, which are not
  forwarded and so never reach `rx_packets`), or `rx_dropped_ring`.
- `rx_dropped_partial` and `rx_dropped_reassembly` count **reassemblies** that
  were given up, not frames.

So a node's total received traffic is `rx_packets + beacons_rx` plus whatever
the loss counters record, and a split packet contributes two frames to the air
but one to those totals.

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
"csma_slot_ms":25,"csma_band":1}`. `regime` names the rulebook the node's region puts it under, and `band` is the
sub-band within it where one exists. Only the EU plan has sub-bands to report,
so at 2.4 GHz or in the US band `band` repeats the region name and
`band_limit_pct` is 0 — those regimes cap something other than a share of the
hour, and `duty_limit_pct` is 0 with them for the same reason.

`band` and `band_limit_pct` are what the
regulator allows for the channel — chosen from the sub-bands the carrier
actually occupies at the configured bandwidth, taking the stricter one when it
straddles a boundary. `band_allocated` is false on the ranges between the
sub-bands, which are not meant for this kind of device and get the strictest
allowance in the plan. `duty_limit_pct` is what the node enforces (the
allowance less a safety margin, or a stricter `duty_manual_pct` when set);
`long_pct` is the share of the last hour spent transmitting and `locked` says
transmissions are being held until `retry_after_s` seconds from now. The CSMA
slot and contention-window band come from the same figures.

`board` is the make and model this firmware was built for. A node's name is
derived from its MAC, so a bench of eight nodes is eight hex strings; this is
the half a person can match to the board in front of them, and the portal puts
it in the page header and in the browser tab. `GET /api/settings` carries it
too, so the page that changes a node's settings can name the node it is about
to change.

`power` reports the profile, CPU clock and the cell:
`{"profile":"performance","cpu_mhz":240,"battery_present":true,
"battery_charging":true,"battery_v":3.53,"battery_pct":10,"pmu":"AXP2101"}`.
The board's make and model used to sit here too; it is `board` at the top of
the document now, beside the firmware that runs on it. `battery_charging` and a trustworthy
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

`sd` also says what the card in the slot holds and what may be done with it:
`store_home` (`"sd"` or `"littlefs"`), `card` (`none`, `blank`, `ours`,
`foreign` — another node's store, or a marker that cannot be read — or
`legacy`, a store written before cards were marked), `owner`/`generation` from
the card's marker, `migrating` while a move is queued or running, and
`migration`, the result of the last one. `can_adopt` and `can_eject` are the
node's own answer about whether it would accept each move; a page draws its
buttons from those rather than working the rule out again from the fields above.

### Local links
`local_links` lists every way a host can reach the node — see
[local-link.md](local-link.md). `type` is `wifi_ap`, `wifi_sta`, `usb_ncm` or
`ppp_uart`; `phase` is `disabled`, `down`, `up` (carrier, no address) or
`ready`; `addressing` is `static` (the node's own number: the access point,
usb0), `dhcp` (leased to the node: the station uplink), `ipcp` (assigned by
the PPP peer: ppp0, where the node is the client and the host's pppd
decides) or `none`. `hardware`
says the board has it, `firmware` that this build can run it, `enabled` that
the operator has it on; when the first is true and the second false, `reason`
says why. `clients` is present only where the link can count its hosts.
Byte counters will appear with a driver that can produce them; the Wi-Fi
stack cannot, and a field that is always null is not worth polling.

## System (auth, POST only, local links only)
The privileged operations, guarded by the admin password, the
`maintenance.bootloader_api` switch and the link the request came over: by
default only a **host-facing** link — the access point, USB, PPP — is
accepted, and a request from the station network answers `403` unless
`maintenance.bootloader_from_lan` is set. Nothing here is reachable through
Reticulum. Details and the recovery procedure: [local-link.md](local-link.md#the-bootloader-manager).

- `GET /api/system/bootloader` (public) → `{ "software_entry", "api_enabled", "pending", "state", "primary", "methods": ["software_api","auto_reset_dtr_rts","manual_recovery"], "recovery", "board", "confirm": "BOOTLOADER", "allowed_from_here" }` — what this board can do and whether a request from the caller's link would be accepted. While `pending` is true the object also carries `target` (`app` or `bootloader`), `source` (`http`, `console`, `settings` or `touch`) and `due_in_ms`, the time left before the restart fires. After a restart the object carries `last_restart` — `to_persist_ms` and `to_boot_ms`: the milliseconds from entering the restart to handing over to the core's persist-restart (native USB only), and from there to this boot, kept in RTC memory across the ROM session — so a restart that took the long way round can say where it went. The first seven fields are the same object `GET /api/settings` returns as `bootloader`; "from here" is decided by the link the request arrived on, not by the caller's address
- `POST /api/system/bootloader` `{"confirm":"BOOTLOADER"}` → `202 { "ok":true, "restart":true, "target":"bootloader", "method":"software_api", "delay_ms":600, "expect":"…", "recovery":"…" }`; the node restarts into the ROM downloader 600 ms later. `400` without the confirm word, `403` when switched off or from a non-local link, `409` while a restart is already in progress, `500` when the request was accepted but the ROM could not be armed (the node restarts into the application instead), `501` where this board cannot (a classic ESP32, or a native-USB S3 built without the composite device, whose serial-JTAG unit survives the reset; use esptool's DTR/RTS reset — the shipped S3 images present the composite device and answer `202`). The console's `BOOTLOADER CONFIRM` answers with the same codes, and over a network session it answers to the same two switches — `bootloader_api`, and `bootloader_from_lan` unless the caller is on a host-facing link. Over the cable neither applies: physical access already allows dumping the firmware and reflashing it
- `POST /api/system/reboot` `{"confirm":"REBOOT"}` → `202 { "ok":true, "restart":true, "target":"app" }`

While a restart is pending every settings `POST` (including `reset`) answers
`503` and new Reticulum TCP connections are refused. `POST /api/system/bootloader`
is the exception: a bootloader request may outrank a plain reboot that is
already armed, and the earlier of the two deadlines is kept. Every reply that
says `"restart"` says whether the restart was actually granted.

## Messages (auth)
`GET /api/messages` → the LXMF messages this node has been sent, newest first.
Behind the admin password, like the settings page: what a node was told is not
public. `/messages.html` is the page that renders it, and is gated the same way.

```json
{ "address": "2aa670cd82b918e2cf5457f46f5e3c44", "stored": 3, "newest": 3,
  "slots": 50, "boot_id": 1140900812, "uptime_ms": 106032, "more": false,
  "messages": [ { "seq": 3, "from": "5f61718a…", "standing": "verified",
                  "via": "link", "sent_at": 1788172420.6,
                  "boot_id": 1140900812, "boot_ms": 85642, "text": "…" } ] }
```

- `standing` is `verified` (this node has heard the sender announce and the
  signature matched), `unverified` (never heard them, so there was no key to
  check against) or `mismatch` (heard them, and the signature did not match).
  The three are kept apart because only the first will ever be allowed to
  drive a privileged action.
- `via` is `packet`, `link` or `resource` — how it reached the node.
- `sent_at` is the sender's own clock, from the message. `boot_ms` is this
  node's `millis()` when it took the message in, and is only comparable with
  `uptime_ms` when `boot_id` matches the document's: `millis()` starts again
  at every restart, so a message from an earlier run has no age, only a date.
- `?n=` how many to return and `?before=<seq>` for the next page; `more` says
  whether another page exists. The cap is what a board can render, not how
  many are kept: 1–16, default 12. Anything else is `400` — the console
  refuses the same values, from the same rule. `slots` is how many the ring
  holds (50), so a client does not have to assume it.
- Arrivals are rate limited before they reach the store and a message the
  sender retransmits is not stored twice; `lxmf_not_stored` in the console's
  `STATUS` counts what was refused. The delivery address is reachable by
  anyone who can reach the node, and every stored message is a flash write.

## Answering a client's ping, echo and signal report (on by default)
Sideband and the clients that follow it have buttons for three questions, sent
in a message's fields rather than as text: **ping**, **echo**, and **signal
report**. The node answers all three with an ordinary message going back, so
they work from an unmodified client with nothing to configure on the phone.

The signal report is the one worth having. It returns what *this node's* radio
measured of the packet that carried the request — link quality, RSSI and SNR,
in Sideband's own layout — so one person with a phone can walk a valley and
find where the node stops hearing them. Without it that takes two people and
two radios. A figure the node does not have is left out rather than reported as
zero, so a request that arrived over Wi-Fi says only what it can.

**Only for a verified sender** — one whose key this node holds and whose
signature over the message matched it. Not because the questions are
privileged, but because a source hash on an unverified message is a *claim*:
the sender wrote it into the payload and nothing checked it, so the answer
would go to whoever the claim named rather than to whoever sent it. Answering
one would make the node a way to put attacker-chosen text, over the node's own
signature, into a stranger's conversation — and to aim its transmitter at a
third party. Remote administration refuses unverified hashes for the same
reason.

That is a different bar from the administrator list, and deliberately lower:
being verified needs only that the node has heard you announce, or that you
identified on the link you opened. The person at the edge of coverage is still
served — a client that has opened a link has proved who it is, which is the
case a signal report is most wanted in.

What they cost is airtime: **one reply per message**, whatever was asked, so
three commands in one message get one answer rather than three. A ping or an
echo is one short packet for one short packet; a **telemetry answer is not** —
the readings run to a couple of hundred bytes against a request of about a
hundred, and can be two fragments at SF12. What bounds that is the per-sender
cooldown of ten seconds, counted only when an answer actually went out, and
the duty-cycle accounting — not the size of the request. `SET
maintenance.lxmf_commands off` (or `"lxmf_commands": false` over HTTP) declines
to spend it on a crowded channel.

### Telemetry

A **telemetry request** is answered with the node's own readings, in the
sensor format Sideband already stores, plots over time and puts on its map.
Nothing is installed at either end, and nothing but the request is needed —
which matters for a node on a hill whose portal nobody can reach.

What it sends is what the board can actually measure:

| Reading | Where it comes from |
|---|---|
| Time | the node's clock |
| Information | firmware version and board |
| Battery | percent, and charging **only where the board can see its charger** |
| Position | the receiver, and only if `radio.gps_share_position` is on |
| Physical link | RSSI, SNR and quality of the last frame heard |
| Processor, RAM, storage | CPU clock, internal heap, LittleFS — and the SD card as a second entry where the store lives there |

A reading the board cannot take is **left out**, never sent as zero: a node
with no cell says nothing about a battery rather than reporting empty, and a
board that cannot see its charger sends "unknown" rather than "not charging" —
which would send somebody looking for a fault in a working cable. A position
is published only with the operator's say-so, under the same setting that
governs the public status API.

Time is sent **only if somebody set the clock**. Nothing here runs NTP, so a
board without a GNSS receiver counts from the epoch at boot — and since a
client files every reading by that timestamp, a wrong one would put the node's
whole history in 1970 rather than being slightly off. The signal figures are
what was heard of *the packet that asked*, carried with the request rather than
re-read at send time, so a request from the edge of coverage is not answered
with the reading of a neighbour who spoke in between.

The document is built when the answer goes out, not when the request arrives,
so the readings are current.

## Remote administration over RNS (off by default)
A node can be administered by messaging it, which is the only way in when its
cable is dead. The delivery address is reachable by anyone who can route to it
and none of those routes authenticate, so a message becomes a command only
after four questions, each of which can only refuse:

1. **Is it on, and is anybody listed?** `maintenance.rns_admin` is off by
   default, and an empty `maintenance.rns_admins` is off however the switch
   reads.
2. **Is the sender proved?** Only `verified` — this node has heard them
   announce, holds their key, and the signature matched. `unverified` (no key
   to check) and `mismatch` (a key that did not match) are both refused.
3. **Are they an administrator?** Verification proves *who*, never *what they
   may do*: anyone can announce. `maintenance.rns_admins` is the authorisation
   — up to four source hashes, 32 hex digits each, comma separated.
4. **Is the message new?** Each administrator's commands must carry a strictly
   increasing timestamp. A signed message stays signed, so one captured off
   the air could otherwise be sent again by anyone who kept the bytes.

What passes goes to the same parser the serial console uses, as a session that
is **not** host-facing — so the vocabulary, the argument checking and the
refusals are the console's, and `BOOTLOADER CONFIRM` declines for the same
reason it declines from the station network. The answer comes back as an LXMF
message, truncated to what fits in one packet.

```
curl -su admin:retimesh -X POST http://10.42.0.1/api/settings/maintenance \
  -d '{"rns_admins":"acb001b427409e8ae73daef126b39866","rns_admin":true}'
```

The floor each administrator has reached is kept in its own store, written
when a command is accepted. It is not derived from the message log: anybody can
put a record there, so a stranger could otherwise date one 2096 under an
administrator's hash and lock the real one out, or fill the ring to erase the
record of a command and replay it after a restart.

A command longer than a console line is refused rather than trimmed, and one
that arrives while the console is switched off is refused with that as the
reason. Refusals reach the sender as `RM ERR ADMIN …`, except the three that
would tell a stranger something they do not already know.

`GET /api/settings` returns both under `maintenance`. The console spells them
`SET maintenance.rns_admins <hash>[,<hash>…]` (`-` clears the list, and so does
`"-"` over HTTP) and `SET maintenance.rns_admin on|off`; both apply at once,
without a restart.
`STATUS` reports `rns_admin=`, how many are listed, how many commands were
offered and run, and the verdict on the last one — which names which of the
four questions it failed rather than leaving them all as "no".

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
- `GET /api/settings` → `{ radio, wifi, transport, links, maintenance, bootloader, admin }` (password never returned; `has_password`, `default_password` flags). `links` is `{ wifi: {hardware, supported, enabled}, usb: {…, reason}, ppp: {…, reason, baud, bauds, node_ip, host_ip} }` — `enabled` is false for a link this build cannot run, whatever is stored; on a board that runs PPP, `baud` is the serial speed while PPP is on, `bauds` the speeds this board may be set to (the registry's ladder up to the rate the board has been tried at — the only list the settings page offers), and `node_ip`/`host_ip` the addresses the node asks its peer for (what the host's pppd is told); `maintenance` is `{ bootloader_api, bootloader_from_lan, console_enabled, console_tcp, web_ui }`; `bootloader` is what the board can do, the same object as `GET /api/system/bootloader`
- `POST /api/settings/radio` `{freq_mhz,bw_khz,sf,cr,tx_dbm,sync_word,preamble,announce_interval,beacon_interval,callsign,duty_cycle_pct,gps_enabled,gps_share_position}` → applied live; `apply_error` in status if the chip rejected it
- `POST /api/settings/wifi` `{ssid,security,password,channel,max_stations,hidden,sta_ssid,sta_password}` → saves, restarts (`"restart":true`); `sta_ssid` blank = station mode off
- `POST /api/settings/transport` `{enabled,lora_mode,wifi_mode,auto_mode,announce_cap,announce_rate_target,announce_rate_grace,announce_rate_penalty,auto_enabled,auto_group_id,power_profile,sd_store}` — the power profile applies live; the other fields restart the node (modes 1 full, 2 gateway, 3 access_point, 4 roaming, 5 boundary; cap in %, rates in s) → saves, restarts
- `GET /api/sd/log` (`?prev=1` for the rotated file) → the SD event log as text
- `GET /api/settings/export` → downloadable JSON of all settings (no identity keys). `links` carries only the links this build can run, and `ppp_baud` where it runs PPP: a switch for a driver that does not exist here would carry a meaningless value onto a node where it means something (an import drops a `ppp_baud` the receiving board is not qualified for, likewise)
- `POST /api/settings/import` (a settings export; sections optional) → applies, restarts
- `POST /api/settings/links` `{wifi,usb,ppp,ppp_baud}` (any subset; the first three booleans, `ppp_baud` an integer) → saves; `"restart":true` when Wi-Fi changed and the restart was granted; `usb`, `ppp` and `ppp_baud` apply live. A link the board lacks or the build cannot run is refused by name with the reason (`400`) rather than saved; so is a combination that, with the console off, would leave no way to reach the node, and so is a `ppp_baud` outside `links.ppp.bauds` (`400 ppp_baud … refused`). A speed change is applied when the console next owns the port, so one saved over ppp0 does not cut off its own reply
- `POST /api/settings/maintenance` `{bootloader_api,bootloader_from_lan,console_enabled,console_tcp,web_ui}` → saves; everything but `web_ui` applies live, and a change to `web_ui` answers `{"ok":true,"restart":true}` and lands at the next boot, since the portal cannot be taken down under the request that asked for it. A combination that would leave no way into the node is refused (`400`): with the console off it needs a link switched on *and* `web_ui`, because the console's own listener (`console_tcp`) goes off with the console. `web_ui:false` is how a board too small for a portal is administered — `28 624 B` of internal RAM on a Heltec Wireless Stick against a few hundred for the console listener that replaces it
- `POST /api/settings/admin` `{password}`
- `POST /api/settings/reset` → factory defaults, restarts (identity kept)
- `POST /api/settings/sd/format` `{"confirm":"FORMAT"}` → erases the SD card and creates one FAT32 volume; poll `sd.state`/`sd.last_format` in `/api/status` (409 if no card or already formatting)
- `POST /api/settings/sd/adopt` `{"confirm":"ADOPT"}` → moves the Reticulum store onto the card and restarts into it; the copy is made during the restart, with nothing holding the store open (409 with the reason when refused — see `sd.can_adopt`)
- `POST /api/settings/sd/eject` `{"confirm":"EJECT"}` → moves the store back to internal flash the same way and marks the card released, after which any node may take it (409 with the reason when refused — see `sd.can_eject`)

Errors: `{"error":"<reason>"}` with 400/401/403/409/413/500/501/503.

## Captive portal
`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, … and any
unknown path redirect to `http://10.42.0.1/`.
