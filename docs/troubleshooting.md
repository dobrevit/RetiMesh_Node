# Troubleshooting

**Console:** the USB port at 115200 baud prints everything (`pio device
monitor` or any terminal). Boot lines to expect: identity, `SSD1306 found`,
`SoftAP … up`, `SX12xx online`, `Reticulum transport up`.

| Symptom | Cause | Fix |
|---|---|---|
| `SX1262 init failed, code -2` / `No LoRa transceiver found` | wrong pins/board variant, or a v0.0.1 build on an SX127x board | ≥ v0.0.2 auto-detects; check wiring/`PIN_LORA_*` for custom boards; TCXO voltage for SX1262 modules |
| OLED shows the previous firmware's screen | panel never re-initialised | ≥ v0.0.2 re-inits at boot |
| Web flasher needs BOOT held | previous firmware's USB stack ignored the reset sequence | one-time; RetiMesh firmware resets hands-free |
| `/dev/ttyACMx` numbers changed after replugging | Linux enumerates in plug order | use `/dev/serial/by-id/…` in rnsd config and tools |
| Sideband's Announce stream is empty | Wi-Fi clients in `access_point` mode (default) never receive announces; or the remote announces a non-LXMF aspect (`rnid -a`) which Sideband hides | set Wi-Fi clients mode to `full`; announce an `lxmf.delivery` destination (NomadNet/Sideband/lxmd) on the remote |
| Neighbour list empty although an RNode is nearby | an RNode only transmits its station ID after real traffic; plain `rnsd` never announces | give it traffic or run NomadNet; set `id_interval`/`id_callsign` |
| rnsd counts "protocol violations" | raw station-ID callsigns from RNodes are not valid packets (RNS design) | harmless; disable IDs on the RNode if it bothers you; RetiMesh beacons/announces do not count |
| A remote announce never reaches the phone | phone connected after the announce (RNS does not replay), or client mode `access_point` | reconnect and wait for the next announce, or use path requests (message by address) |
| Sideband does not find the node via Local/LAN | AutoInterface disabled on either side, different group id, or the phone's Wi-Fi blocks multicast (some Android power-saving modes) | check *Connect via Local/LAN* in Sideband and *Zero-config peering* on the node; group ids must match; fall back to TCP `10.42.0.1:4242` |
| Settings page asks for a password | admin auth | `admin` / `retimesh` by default; factory reset restores it |
| Wi-Fi security `wpa3` greyed out | ESP-IDF 4.4 core has no SoftAP SAE | use WPA2; WPA3 arrives with the core-3 migration |
| Node reboots after saving Wi-Fi/transport settings | by design — those need re-registration | reconnect to the (possibly new) SSID |
| `esp_littlefs: Failed to unlink … Has open FD` | microStore rotates a file it holds open | harmless; tracked upstream |
| SD card shows `partial` | the FAT volume covers only part of the card (Raspberry Pi image, phone card) | *Format card* on the settings page (erases it) |
| SD card shows `unformatted` | ext4/exFAT/other filesystem, or a blank card | same — format from the settings page |
| Heap keeps dropping | possible leak | report with the heartbeat lines (`up … heap …`) and the version |

Still stuck? Open an issue with: board and revision, firmware version (`/api/status`),
the boot log, and what the peers are running (RNS/Sideband/NomadNet versions).
