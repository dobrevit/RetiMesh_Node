# Troubleshooting

**Console:** the USB port at 115200 baud prints everything (`pio device
monitor` or any terminal) and answers commands — type `HELP`, `VERSION`,
`STATUS` or `NETWORK_STATUS`; replies start with `RM `. Boot lines to expect:
`RM HELLO …`, identity, `SSD1306 found`, `SoftAP … up`, `SX12xx online`,
`Reticulum transport up`.

**Recovery, always:** hold BOOT, press RST (or replug USB while holding BOOT),
release BOOT, flash with `retimesh-flash install` or esptool. Nothing the
firmware does can prevent this — the bootloader bit it sets is cleared by the
ROM, and a power cycle boots whatever is in flash.

| Symptom | Cause | Fix |
|---|---|---|
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
| Sideband's Announce stream is empty | Wi-Fi clients in `access_point` mode (default) never receive announces; or the remote announces a non-LXMF aspect (`rnid -a`) which Sideband hides | set Wi-Fi clients mode to `full`; announce an `lxmf.delivery` destination (NomadNet/Sideband/lxmd) on the remote |
| Neighbour list empty although an RNode is nearby | an RNode only transmits its station ID after real traffic; plain `rnsd` never announces | give it traffic or run NomadNet; set `id_interval`/`id_callsign` |
| rnsd counts "protocol violations" | raw station-ID callsigns from RNodes are not valid packets (RNS design) | harmless; disable IDs on the RNode if it bothers you; RetiMesh beacons/announces do not count |
| A remote announce never reaches the phone | phone connected after the announce (RNS does not replay), or client mode `access_point` | reconnect and wait for the next announce, or use path requests (message by address) |
| Messages to a peer fail until Sideband is restarted (ratchet errors) | Sideband encrypts to the peer's announced ratchet key and caches it; after the peer's keys/ratchets change it can hold a stale one | wait for the peer's next announce or restart Sideband; the node relays announces with their ratchets unchanged (transport rebroadcasts preserve the context flag and ratchet bytes) |
| Sideband does not find the node via Local/LAN | AutoInterface disabled on either side, different group id, or the phone's Wi-Fi blocks multicast (some Android power-saving modes) | check *Connect via Local/LAN* in Sideband and *Zero-config peering* on the node; group ids must match; fall back to TCP `10.42.0.1:4242` |
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
