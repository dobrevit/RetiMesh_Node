#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Receive RetiMesh beacons.

Beacons are Reticulum broadcasts to the PLAIN destination retimesh.beacon
with a printable payload "RM1 <T> <callsign> <version>" (T = H hello,
R reply, I periodic). They are off by default on the node (settings page,
beacon interval > 0 to enable).
"""
import time
import RNS

RNS.Reticulum()
dest = RNS.Destination(None, RNS.Destination.IN, RNS.Destination.PLAIN, "retimesh", "beacon")


def on_packet(data, packet):
    iface = packet.receiving_interface
    rssi = getattr(packet, "rssi", None)
    print(f"{time.strftime('%H:%M:%S')}  {data.decode(errors='replace')!r}  via {iface}"
          + (f"  rssi={rssi}" if rssi is not None else ""))


dest.set_packet_callback(on_packet)
print(f"Listening on retimesh.beacon <{dest.hash.hex()}> (Ctrl+C to stop)…")
while True:
    time.sleep(1)
