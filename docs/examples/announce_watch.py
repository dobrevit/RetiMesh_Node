#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print every announce this Reticulum instance receives.

Run on any machine with an RNS interface to the mesh (a laptop on the
node's Wi-Fi, or an rnsd with an RNode). RetiMesh nodes announce
lxmf.delivery and nomadnetwork.node; builds up to v0.0.x also announced
retimesh.node with app_data "<callsign> <version>".
"""
import time
import RNS

KNOWN = {
    RNS.Identity.full_hash(b"retimesh.node")[:10]:     "retimesh.node",
    RNS.Identity.full_hash(b"lxmf.delivery")[:10]:     "lxmf.delivery",
    RNS.Identity.full_hash(b"lxmf.propagation")[:10]:  "lxmf.propagation",
    RNS.Identity.full_hash(b"nomadnetwork.node")[:10]: "nomadnetwork.node",
}


class Handler:
    aspect_filter = None      # all aspects
    receive_path_responses = True

    def received_announce(self, destination_hash, announced_identity, app_data, announce_packet_hash=None):
        try:
            text = app_data.decode("utf-8") if app_data else ""
        except UnicodeDecodeError:
            text = app_data.hex()
        hops = RNS.Transport.hops_to(destination_hash)
        print(f"{time.strftime('%H:%M:%S')}  <{destination_hash.hex()}>  "
              f"identity <{announced_identity.hash.hex()}>  hops={hops}  app_data={text!r}")


RNS.Reticulum()
RNS.Transport.register_announce_handler(Handler())
print("Watching announces (Ctrl+C to stop)…")
while True:
    time.sleep(1)
