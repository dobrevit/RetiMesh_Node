# Examples

| File | Use |
|---|---|
| [rnsd-client.conf](rnsd-client.conf) | `~/.reticulum/config` snippet for a laptop/rnsd on the node's Wi-Fi |
| [rnode-peer.conf](rnode-peer.conf) | RNodeInterface block for an RNode on the same LoRa channel |
| [remote-node-nomadnet.md](remote-node-nomadnet.md) | set up a remote LXMF peer (rnsd + RNode + NomadNet) to message through the node |
| [announce_watch.py](announce_watch.py) | print every announce your RNS instance hears (spot RetiMesh nodes and their peers) |
| [beacon_listener.py](beacon_listener.py) | receive RetiMesh beacons (`retimesh.beacon` PLAIN destination) |
| [api.sh](api.sh) | curl one-liners for the HTTP API (status, board, settings) |

Python examples use the `RNS` package (`pip install rns`) and attach to a
running `rnsd` or start their own instance from `~/.reticulum/config`.
