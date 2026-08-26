# A remote LXMF peer to message through the node

Goal: a second site with an RNode, reachable from a phone on the RetiMesh
node's Wi-Fi.

1. On the remote machine, install and configure RNS with the RNode on the
   node's channel — see [rnode-peer.conf](rnode-peer.conf) — and enable
   transport if that machine should route further:
   ```ini
   [reticulum]
     enable_transport = Yes
   ```
2. Start `rnsd`. `rnstatus` must show the RNode interface `Up`.
3. Install an LXMF client that announces a delivery destination:
   ```sh
   pip install nomadnet
   nomadnet            # TUI; or `nomadnet --daemon` headless
   ```
   NomadNet announces at start and periodically (`~/.nomadnetwork/config`,
   `announce_at_start`, `announce_interval`). Its LXMF address is under
   *Conversations*.
4. On the phone (Sideband, connected to the node over TCP), the remote peer
   appears in the announce stream when the node's *Wi-Fi clients mode* is
   `full`; in `access_point` mode, add the address manually — Sideband will
   path-request it through the node.
5. Message it. On the node's console you will see the path request, the
   forwarded packets and the link; `rnpath -t` on the remote lists the phone's
   destination *2 hops away via* the node's transport identity.

Plain `rnsd` never announces by itself, and `rnid -i <identity> -a <aspect>`
announces a non-LXMF aspect that Sideband does not display — use it only to
test the node's neighbour list / path table.
