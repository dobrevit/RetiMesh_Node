# Getting started

## 1. Flash the firmware

**Browser (Chrome/Edge):** open <https://dobrevit.github.io/RetiMesh_Node/>,
pick your board, click *Install*, choose the serial port. Tick *Erase device*
on a first install. If the port does not show up, hold **BOOT**, tap **RST**,
release **BOOT**, retry.

**Terminal:**
```sh
pipx run --spec "git+https://github.com/dobrevit/RetiMesh_Node#subdirectory=tools/retimesh-flash" retimesh-flash install
```
It lists boards, auto-detects the port, verifies checksums and flashes.

**Manual:** download `retimesh-node-<version>-<board>-merged.bin` from the
release and `esptool.py --chip esp32s3 write_flash --erase-all 0x0 <file>`.

## 2. Join the node

The node starts an open Wi-Fi network named **`retimesh-XXXXXX`** (last three
octets of its MAC — also shown on the OLED). Join it; the captive-portal page
opens at <http://10.42.0.1/> (open it manually if your OS does not pop it up).

The status page (also at <http://retimesh.local/>) shows the radio (model,
channel, RSSI/SNR), transport interfaces and known paths, neighbours heard on
the channel — LXMF peers get a *Message in Sideband* link and an address copy
button — and a public bulletin board.

## 3. Connect Sideband (or any Reticulum client)

**Zero-config:** with Sideband's *Connectivity → Connect via Local/LAN* enabled
(its default), the phone finds the node within a few seconds of joining the
Wi-Fi — nothing to type. Every Reticulum client with an AutoInterface (rnsd,
NomadNet, MeshChat on a laptop) does the same.

**Manual:** *Connect via TCP* → host `10.42.0.1`, port `4242`.
For `rnsd`/NomadNet on a laptop on the same Wi-Fi, add to `~/.reticulum/config`:

```ini
[[RetiMesh Gateway]]
  type = TCPClientInterface
  enabled = yes
  target_host = 10.42.0.1
  target_port = 4242
```

Every client gets its own transport interface on the node (default mode
`full`). Within seconds the node announces itself and Sideband learns paths
through it.

## 4. Send a message

Message any LXMF peer reachable over the LoRa channel — another RetiMesh node's
phone, or a remote `rnsd` + NomadNet with an RNode on the same channel
parameters (default **868.100 MHz, BW 125 kHz, SF8, CR 4/5**). Paths are found
automatically; the first message may take a few seconds while the path request
crosses the air.

Remote peers appear in Sideband's *Announce stream* as their announces arrive;
Sideband lists LXMF and NomadNet aspects only, so the stream fills once a real
LXMF peer announces rather than on the node's own `retimesh.node`. Setting
*Client interface mode* to `access_point` withholds announces from phones
deliberately — see [reticulum.md](reticulum.md#interface-modes).

## 5. Administer

<http://10.42.0.1/settings.html> — user `admin`, password **`retimesh`**
(change it there). Radio channel (applied live), Wi-Fi security/SSID, transport
modes, admin password, factory reset. Radio parameters must match every other
node on the channel, including RNodes.
