# Reticulum integration

## Interfaces
| Interface | Where | Framing | Notes |
|---|---|---|---|
| **LoRa** | radio | RNode-compatible: 1 header byte (random nibble + split flag), ≤255-byte frames, two-fragment packets up to the 500-byte RNS MTU | byte-compatible with RNode firmware — real RNodes on the same channel parameters interoperate |
| **TCP :4242** | Wi-Fi | RNS TCPInterface HDLC (`0x7E` flags, `0x7D` escapes) | one RNS interface per connected client, like `rnsd`'s TCPServerInterface |
| **Station uplink** | Wi-Fi (STA) | same TCP :4242 and AutoInterface, on your LAN | with a station network configured, Reticulum clients on the LAN reach the node at its LAN address or by discovery — the node is a LoRa uplink for the whole network |
| **AutoInterface** | Wi-Fi, on the access point and the LAN | RNS AutoInterface: IPv6 link-local discovery — multicast (UDP 29716) and unicast reverse peering (29717) — packets as UDP datagrams (42671) | zero-config — Sideband's *Local/LAN* finds the node by itself; one RNS interface per peer, under the *Peer interface mode* |

## Identity and announces
The node has a persistent Reticulum identity (X25519 + Ed25519, NVS) used
both as **transport identity** and for its `retimesh.node` destination. It
announces on boot, a couple of seconds after a client or peer registers, and
every `announce_interval` (default 10 min) — all *through Transport*, so
interface modes apply. The announce on registration is what a phone or a node
that has just appeared hears; without it the first announce it could see was up
to an interval away. `/api/status` shows `identity` and
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

**Persistence.** The path table, known destinations and packet hash list are
kept in a microStore under `/rns`, on the SD card when one has been taken into
use and in internal flash otherwise — one home at a time, moved deliberately
rather than followed automatically. See
[Architecture](architecture.md#the-store-has-one-home). The node identity lives
in NVS either way and survives a factory reset, so a node keeps its address even
when its store is wiped.

## Interface modes
Exactly rnsd's vocabulary; each interface has its own.

| Mode | Announces onto it | Use |
|---|---|---|
| `full` | all | the mesh backbone; the default for all three kinds below |
| `gateway` | not from other gateway interfaces | node bridging distinct networks |
| `access_point` | **none** — clients discover via path requests; short path expiry | phones that come and go, when announces are to be withheld from them |
| `roaming` | limited; shorter path lifetime | a node that moves between networks |
| `boundary` | not from roaming/internal | edge of an administrative domain |

The node has three kinds of neighbour and a mode for each: the **LoRa
interface**, the **clients** that connect to :4242, and the **AutoInterface
peers** it discovers on the Wi-Fi links. They are separate settings because
they are separate policies — one value covering both Wi-Fi kinds meant that
choosing `access_point` for phones also stopped this node exchanging announces
with the other nodes on its LAN.

Practical consequence: an interface in `access_point` sends no announces at
all, so Sideband's *Announce stream* stays empty on a node whose clients are in
that mode, and a room of nodes whose peers are in that mode never learns of
each other. Messaging still works through path requests, which is what the mode
is for. The log says so once at boot for each kind that is set to it.

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
