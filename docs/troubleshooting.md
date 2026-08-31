# Troubleshooting

**Console:** the USB port at 115200 baud prints everything (`pio device
monitor` or any terminal) and answers commands — type `HELP`, `VERSION`,
`STATUS` or `NETWORK_STATUS`; replies start with `RM `. The same console
answers on TCP :4243 over any link the node has, where it wants the admin
password first (`AUTH …`). `tools/console.py <port-or-host> STATUS` takes
either, and unlike a terminal it does not reset the board when it opens the
cable. It is the way in when the portal is off, or gone. Boot lines to expect:
`RM HELLO …`, identity, `SSD1306 found`, `SoftAP … up`, `SX12xx online`,
`Reticulum transport up`.

**Recovery, always:** hold BOOT, press RST (or replug USB while holding BOOT),
release BOOT, flash with `retimesh-flash install` or esptool. Nothing the
firmware does can prevent this — the bootloader bit it sets is cleared by the
ROM, and a power cycle boots whatever is in flash.

| Symptom | Cause | Fix |
|---|---|---|
| The portal will not open, connection refused, but the node answers otherwise | `maintenance.web_ui` is off — the routes are never registered and nothing listens on port 80. Deliberate on a board too small to host a portal | reach it over the console instead (`tools/console.py <host>`), and `SET maintenance.web_ui on` if you want it back; it restarts to apply |
| Asking for the portal *reboots* the node | it ran out of byte-addressable RAM serving the page: `esp_littlefs: Unable to allocate FD`, then `abort()`, then a boot with `reason="panic or unhandled exception"` | the page did not fail, the request did. Check `dram_free` and `dram_largest_block`; on a tight board switch the portal off and use the console. See the row below |
| Plenty of heap reported, but a task will not start or an allocation fails | `heap_free` counts 32-bit-only IRAM that no byte-addressed allocation can use — a board can read 46 KB free with 4 KB usable | read `dram_free`, `dram_min`, `dram_largest_block`: those decide. The boot log bills every subsystem (`cost: …`), so what is spending the RAM is a line to read rather than a guess |
| The TCP console refuses everything with `401` | the session has not authenticated; only `VERSION` and `AUTH` work before it does | `AUTH <admin password>` — the same password the web API takes |
| The TCP console answers `429 too many attempts` | three wrong passwords; the node stops answering `AUTH` for 30 s, counted for the node rather than the connection, so reconnecting does not clear it | wait it out, or use the cable, which is never locked out |
| The TCP console answers `503 another session has the console` | one caller at a time, by design | close the other session, or wait: an idle one is dropped after 2 minutes, an unauthenticated one after 20 s |
| The portal is missing a control the firmware supports, or names a wrong version | firmware and filesystem came from different builds — `-t upload` writes only the application | `-t uploadfs` as well; `/api/status` `assets.match` says whether the halves agree. On a board with no SD card this erases the Reticulum store and its identity |
| `SX1262 init failed, code -2` / `No LoRa transceiver found` | wrong pins/board variant, or a v0.0.1 build on an SX127x board | ≥ v0.0.2 auto-detects; check wiring/`PIN_LORA_*` for custom boards; TCXO voltage for SX1262 modules |
| OLED shows the previous firmware's screen | panel never re-initialised | ≥ v0.0.2 re-inits at boot |
| Web flasher needs BOOT held | previous firmware's USB stack ignored the reset sequence | one-time; RetiMesh firmware resets hands-free |
| `pio run -t upload` says `several ESP32 ports` | more than one board attached and no port chosen | `--upload-port /dev/serial/by-id/…` (by-path for CP2102 boards, which all report serial `0001`) |
| `pio run -t upload` says the port `did not reappear` | the node accepted the bootloader request but its USB device did not come back within 8 s — a hub or host that re-enumerates slowly | hold BOOT, press RST, run the upload again; `RETIMESH_NO_AUTO_BOOTLOADER=1` leaves the reset to esptool |
| `POST /api/system/bootloader` answers 403 | the request came over the station network, or the API is switched off | ask over the access point/USB, or set *also from the station network* / *Bootloader API* on the settings page |
| `POST /api/system/bootloader` answers 501, console says `RM ERR BOOTLOADER 501` | a classic ESP32 (T-Beam, Wireless Stick): no `FORCE_DOWNLOAD_BOOT` bit | esptool resets it through the bridge's DTR/RTS; nothing to fix |
| Opening the serial console resets the node | the terminal drops DTR or RTS after opening; both low, or RTS alone, is the reset handshake on USB-Serial/JTAG and pulls EN on a bridge | leave both lines asserted, which is the running state: `pio device monitor` does since `monitor_dtr`/`monitor_rts` are set in platformio.ini, and `retimesh-flash devices` opens the port that way |
| Wi-Fi is gone after a settings change and the page is unreachable | Wi-Fi was switched off under *Local links* | `WIFI ON` on the serial console (saves and restarts) |
| Node ignores `BOOTLOADER` typed on the console | it wants the word `CONFIRM` — `BOOTLOADER CONFIRM` | as the `RM ERR … 400` line says |
| `/dev/ttyACMx` numbers changed after replugging | Linux enumerates in plug order | use `/dev/serial/by-id/…` in rnsd config and tools |
| After flashing a T3-S3 the `Espressif USB JTAG/serial debug unit` is gone and a `RetiMesh Node` device (1209:0001) is there, with an `enx…` network interface | that is the firmware: the chip's USB is now the composite device — a console port and an Ethernet link — see [local-link.md](local-link.md#native-usb-the-composite-device) | use `/dev/serial/by-id/usb-RetiMesh_RetiMesh_Node_<mac>-if00` for the console and `http://10.64.<n>.1/` over the cable |
| A native-USB board shows a blank display and only the `USB JTAG/serial debug unit` port after a flash with another tool | it was left in the ROM downloader: a downloader entered from software stays there through esptool's hard reset unless esptool's connect-time reset ran first | `esptool --chip esp32s3 --port <that port> --before default_reset --after hard_reset chip-id`, or press RST; the hook and `retimesh-flash` do this themselves |
| `pio run -t upload` says `no RetiMesh console` on a node that is plainly running, and the first console command after plugging in answers `RM ERR ? 400` | something wrote to the port without a newline — ModemManager probes every new CDC-ACM port with `AT` | the node drops a partial line the port has been silent on for ten seconds, and the tool asks again; install `tools/udev/60-retimesh-node.rules` to stop the probing (it also explains why a first request right after plugging in can go unanswered) |
| `pio run -t upload` on a native-USB board sits at `still waiting for the serial-JTAG downloader` for up to a minute | the node is behind a hub that reports a full-speed device's departure only once a transfer to it fails; the chip has been in its ROM since two seconds after the request | wait — the tool does, for three minutes — or put the node on a root port, where the downloader appears within a second |
| The host got a route or a DNS server via the USB link | the lease names the node as DNS (ESP-IDF's DHCP server always names one); it carries no router | nothing to fix: the node refuses DNS over the cable and offers no default route, so the resolver moves on and traffic stays where it was |
| The serial console stops answering, and the log goes quiet, while PPP is up | PPP owns the port: a log line inside a frame would corrupt it, so the log is muted and the console suspended for as long as the host has PPP open ([local-link.md](local-link.md#ppp-over-the-bridge-uart)) | expected. Use `http://10.65.<n>.1/` and the API meanwhile; the console returns when pppd exits, when `ppp` is switched off over HTTP, or after 30 s without a frame from the host, and the first log line then says why |
| `pppd` sends `LCP ConfReq` ten times and gives up (`timeout sending Config-Requests`), the node never answers, and the serial console keeps working meanwhile | hardware flow control is on for the tty — Debian's `/etc/ppp/options` sets `crtscts` — and the CP2102 is waiting for a CTS the board never drives, so pppd's frames never leave the bridge | add `nocrtscts` to the pppd command or the peers file, as the tool prints them |
| `pppd` says it must be run as root, or `retimesh-flash` prints a `sudo pppd …` line instead of connecting | pppd needs root on most distributions (or the `dip` group and a setuid pppd on Debian's) | run the printed command with `sudo`, or install the peers file the tool also prints once (then `pppd … call retimesh …` needs no root for members of `dip`); the tool never runs pppd or kills it for you, it prints exactly what to run |
| `POST /api/settings/links` answers `ppp_baud … refused`, or the settings page offers only 115200 | the speed is not one this board is qualified for: `boards.json` lists the ladder (`uart.qualification`) and the highest rate tried (`uart.tested_max_baud`, 115200 on every board today) | pick a speed from `links.ppp.bauds`; raise `tested_max_baud` in `boards.json` only after running the board at the higher rate |
| The serial monitor shows garbage after PPP was switched on with a faster speed | the port runs at `ppp_baud` while PPP is on — the console and the log included | open the monitor at that speed; with PPP off the port is back at 115200 |
| `retimesh-flash install` says `pppd (pid …) still holds /dev/ttyUSB0` or `stop pppd first` | the node was asked for its bootloader over ppp0 but pppd did not exit (started with `persist`, or a classic ESP32 that answered `501` and needs esptool's own reset, which cannot run while pppd has the port) | `sudo kill <pid>` as the message says, then run the install again; restart pppd afterwards with the command the tool prints |
| Sideband's Announce stream is empty | Wi-Fi clients set to `access_point` receive no announces at all; or the only announces are non-LXMF aspects (`rnid -a`), which Sideband hides — the node's own `retimesh.node` is one of them | check *Client interface mode* is `full` (the default; the boot log names any kind of neighbour a mode is withholding announces from); announce an `lxmf.delivery` destination (NomadNet/Sideband/lxmd) on the remote |
| Neighbour list empty although an RNode is nearby | an RNode only transmits its station ID after real traffic; plain `rnsd` never announces | give it traffic or run NomadNet; set `id_interval`/`id_callsign` |
| rnsd counts "protocol violations" | raw station-ID callsigns from RNodes are not valid packets (RNS design) | harmless; disable IDs on the RNode if it bothers you; RetiMesh beacons/announces do not count |
| A remote announce never reaches the phone | phone connected after the announce (RNS does not replay), or client mode `access_point` | reconnect — the node announces itself a couple of seconds after a client registers — and wait for the remote's next announce, or use path requests (message by address) |
| Messages to a peer fail until Sideband is restarted (ratchet errors) | Sideband encrypts to the peer's announced ratchet key and caches it; after the peer's keys/ratchets change it can hold a stale one | wait for the peer's next announce or restart Sideband; the node relays announces with their ratchets unchanged (transport rebroadcasts preserve the context flag and ratchet bytes) |
| Several nodes on one LAN do not see each other | *Peer interface mode* set to `access_point` silences announces between them; or nothing has peered — an access point that does not forward link-local multicast between its clients stops discovery | `/api/status` → `transport.autointerface.peer_list` says who has peered and how long ago each was heard; peerings survive a link that drops multicast because the node also peers by unicast, but the first discovery of a peer still needs one multicast datagram through |
| Sideband does not find the node via Local/LAN | AutoInterface disabled on either side, different group id, or the phone's Wi-Fi blocks multicast (some Android power-saving modes) | check *Connect via Local/LAN* in Sideband and *Zero-config peering* on the node; group ids must match; `peer_list` in `/api/status` shows whether the phone peered at all; fall back to TCP `10.42.0.1:4242` |
| Settings page asks for a password | admin auth | `admin` / `retimesh` by default; factory reset restores it |
| Wi-Fi security `wpa3` greyed out | the firmware was built on an ESP-IDF 4.4 core, which has no SoftAP SAE | flash a current build (core 3.x / IDF 5), or use WPA2 |
| Node reboots after saving Wi-Fi/transport settings | by design — those need re-registration | reconnect to the (possibly new) SSID |
| `esp_littlefs: Failed to unlink … Has open FD` | microStore compaction deleting a segment it still holds open (fixed in our fork, upstream PR attermann/microStore#6) | update the firmware; harmless on older builds |
| SD card shows `partial` | the FAT volume covers only part of the card (Raspberry Pi image, phone card) | *Format card* on the settings page (erases it) |
| SD card shows `unformatted` | ext4/exFAT/other filesystem, or a blank card | same — format from the settings page |
| *Format card* reports `failed: write error` and the card is untouched | before v0.0.8 the wipe ran before anything had woken the card, so the first sector write was refused and the format was never reached — it failed identically on every card | update the firmware |
| An empty slot reports a card, sometimes of an absurd size | before v0.0.8 the presence check treated getting a driver slot as proof of a card and read the capacity from a field nothing had written | update the firmware; an empty slot now reports `absent` with size 0 |
| *Use this card* / *Eject* comes back with `no card in the slot when the node restarted` | the move is performed at the next boot, and the card was removed in between | put the card back and repeat; nothing was moved, the store is still where it was |
| The card reads as belonging to another node | its ownership marker names a different identity — including this node's own identity from before a factory reset, since a reset regenerates it | format the card if you mean to take it; there is no way to merge two stores |
| Settings form refuses to change *Reticulum store on SD* | moving the store means copying it, which that form does not do — saving the flag alone used to leave the node reading an empty store | use *Use this card* / *Eject* under SD card |
| Battery shows *charging unknown* | the board has a voltage divider but no PMU, so nothing tells it whether the charger is working | expected on T3-S3 and similar; the charger still works, the processor just cannot see it |
| A node keeps routing but stops answering HTTP and mDNS, without rebooting | heap fragmentation: the node can still forward packets but can no longer allocate a contiguous block big enough to build a status response | check `heap_largest` against `heap_free` in `/api/status` — a largest block far below free heap is the signature; worst on boards with no PSRAM |
| Heap keeps dropping | possible leak | report with the heartbeat lines (`up … heap …`) and the version |
| `Guru Meditation Error … Unhandled debug exception` with a `0xa5a5a5a5` frame | a task overflowed its stack (stack-canary watchpoint) | the heartbeat's `stack headroom:` line names each task's spare bytes; report it with the panic text |

Still stuck? Open an issue with: board and revision, firmware version (`/api/status`),
the boot log, and what the peers are running (RNS/Sideband/NomadNet versions).

## A node that answers nothing but is not dead

A node can end up with one of its tasks stopped while the others keep running.
Two have been seen doing it, and they look different from outside:

- **the Arduino loop task** carries the console, so the cable goes silent;
- **the Reticulum task** drains the ring a client's packets arrive in, so the
  node accepts nothing — for its own address or anyone else's — while its
  radio still logs happily and its web server still answers.

Both are watched, from the radio task. That is not where the watch started: it
was kept in the Reticulum loop until a stall took *that* task down together
with the loop task, and the one thing that could have named the call went quiet
with it. The radio task has outlived every stall so far.

**The node restarts itself if a task stays stopped.** Nothing can unwind a task
spinning above the Arduino loop — it is runnable and holding its core, so
nothing below it will ever run again — so after two minutes the node restarts
rather than sit dead until somebody power-cycles it. That restart is recorded
with source `stall-watch`, so it is not mistaken for one an operator asked for
or for a crash. Detection and recovery are in **every** build; it is one store
per pass.

**The phase names are a debug-build extra.** Without the flag a stall is still
seen, still logged and still recovered — it just says `not built` where it
would have named a call. Build with names when hunting:

```
PLATFORMIO_BUILD_FLAGS=-DRETIMESH_DEBUG pio run -e heltec-wp -t upload
```

The names cost 40 bytes of RAM and about 1.7 KB of flash when built in.

In the log, from the radio task:

```
[E][LoopWatch.cpp] check(): rns: the rns task has not completed a pass for 35817 ms —
  stuck in "reticulum" (entered 35469 ms ago, 202 passes since boot). That task drains
  the ring a client's packets arrive in, so the node will accept nothing while it is stopped
```

It repeats every 30 s while the stall lasts and says so again when the task
comes back. The phases are the calls each task makes:

| Task | Phases |
|---|---|
| `loop` | `wifi`, `bootloader`, `console-tcp`, `console`, `links`, `inbox`, `rns-admin`, `leds`, `diag`, `delay` |
| `rns` | `log-mute`, `replies`, `events`, `drain-tcp`, `reticulum`, `iface-lora`, `post-iface`, `announce`, `snapshots`, `idle` |

`reticulum`, `iface-lora` and `post-iface` split one library call three ways.
`Reticulum::loop()` runs its own jobs, then every interface's loop, then the
filesystem, then `Transport::loop()`; our LoRa interface is the only part of
that we can label from outside, so `reticulum` means it stalled before the
interfaces ran, `iface-lora` means inside ours, and `post-iface` means after
them — the filesystem loop or Transport's jobs.

**Read it over the network, not the cable.** `GET /api/status` carries the same
thing and keeps answering when neither task does:

```
"tasks": { "loop": { "phase": "delay", "since_pass_ms": 611929, "passes": 7208, "stalled": true },
           "rns":  { "phase": "reticulum", "since_pass_ms": 611402, "passes": 6980, "stalled": true } }
```

`phase: "delay"` on the loop task is worth recognising: that is the task's own
`vTaskDelay`, so it is not blocked in a call — it is not being scheduled, which
points at something else on its core rather than at the call it is in.

`STATUS` on the console prints a `task=` line per stalled task, but only once a
stall is under way — which on a node whose console has gone with the loop task
means it is there for whoever reads a captured log afterwards.

Read `radio.irq_yields` beside them. Nonzero means the radio task was holding
its own core (priority 5, pinned to the core that also carries Reticulum and
the Arduino loop) and had to be forced to yield; that guard is in every build,
not just debug ones.

To prove the watch still works after changing it, add `-DLOOPWATCH_SELFTEST` to
a debug build: the loop task hangs itself 45 s after boot, and the warning, the
repeats and the `/api/status` fields can all be seen.
