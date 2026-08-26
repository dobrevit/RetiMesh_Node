# Reticulum integration

## Interfaces
| Interface | Where | Framing | Notes |
|---|---|---|---|
| **LoRa** | radio | RNode-compatible: 1 header byte (random nibble + split flag), ≤255-byte frames, two-fragment packets up to the 500-byte RNS MTU | byte-compatible with RNode firmware — real RNodes on the same channel parameters interoperate |
| **TCP :4242** | Wi-Fi | RNS TCPInterface HDLC (`0x7E` flags, `0x7D` escapes) | one RNS interface per connected client, like `rnsd`'s TCPServerInterface |
| **AutoInterface** | Wi-Fi | RNS AutoInterface: IPv6 link-local multicast discovery (UDP 29716), packets as UDP datagrams (42671) | zero-config — Sideband's *Local/LAN* finds the node by itself; one RNS interface per peer, sharing the *Wi-Fi clients mode* |

## Identity and announces
The node has a persistent Reticulum identity (X25519 + Ed25519, NVS) used
both as **transport identity** and for its `retimesh.node` destination. It
announces on boot and every `announce_interval` (default 10 min) *through
Transport*, so interface modes apply. `/api/status` shows `identity` and
`destination`; `rnpath -t` on a peer lists the node once its announce arrives.

Announce app_data is `"<callsign> <version>"` — other RetiMesh nodes show it in
their neighbour list; Sideband ignores non-LXMF aspects in its announce stream.

## Transport
microReticulum's Transport (a C++ port of RNS) keeps the path table,
propagates announces with its transport id and hop count, answers path
requests from its table, and forwards link and data packets hop by hop —
the same behaviour as `rnsd` with `enable_transport = yes`. Housekeeping
runs every second. Disable transport on the settings page to run as a plain
bridge (packets are still relayed between the Wi-Fi clients and LoRa, but
nothing is routed or re-announced).

## Interface modes
Exactly rnsd's vocabulary; each interface has its own.

| Mode | Announces onto it | Use |
|---|---|---|
| `full` | all | the mesh backbone; default for LoRa |
| `gateway` | not from other gateway interfaces | node bridging distinct networks |
| `access_point` | **none** — clients discover via path requests; short path expiry | phones that come and go; default for Wi-Fi clients |
| `roaming` | limited; shorter path lifetime | a node that moves between networks |
| `boundary` | not from roaming/internal | edge of an administrative domain |

Practical consequence: with Wi-Fi clients in `access_point`, Sideband's
*Announce stream* stays empty by design; messaging still works (path
requests). Set `full` if you want phones to see announces.

## Talking to an RNode + rnsd
Configure the RNode's interface with the node's channel:
```ini
[[RNode LoRa]]
  type = RNodeInterface
  enabled = yes
  port = /dev/serial/by-id/usb-...        # stable name; ttyACM numbers swap
  frequency = 868100000
  bandwidth = 125000
  txpower = 7
  spreadingfactor = 8
  codingrate = 5
  id_interval = 45                        # optional station ID; shows as a neighbour
  id_callsign = MYCALL
```
`beacon`/`beacon_interval` are **not** RNodeInterface keys. An interface in
`mode = access_point` never transmits announces (RNS rule) — use `full` on the
RNode side if that node should be discoverable.

## Presence extras
- **Station IDs** (`id_callsign`) and **RetiMesh beacons** (`retimesh.beacon`
  PLAIN broadcasts, default off) feed the neighbour list. RNS counts an
  RNode's raw callsign as a malformed packet ("protocol violation") — that is
  RNS's own design; RetiMesh beacons are valid packets and do not.
- Any RNS program can watch RetiMesh beacons by registering the PLAIN
  destination `retimesh.beacon` (see [examples](examples/)).
