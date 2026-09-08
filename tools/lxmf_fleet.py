#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
"""Reaching a fleet of nodes over LXMF: the part that needs a radio.

The soak collector and the metrics exporter ask the same fleet the same two
questions. What differs is what they do with the answers, so the asking lives
here and they keep only their own reading of it.

Two channels, because the interesting numbers are split across them:

  * A **telemetry request** (LXMF FIELD_COMMANDS 0x09, command 0x01) is open to
    any sender and comes back as a readings map — clock, battery, position,
    signal, processor, RAM, storage.

  * A **console line** sent as ordinary message text reaches the same parser
    the cable uses, if this client's identity is enrolled as an administrator
    on the node. That is how STACKS is asked, and per-task stack headroom is
    the measurement a soak exists to take: it only falls, so days of real
    traffic are worth more than any bench session.

Identities are persisted under the storage directory, so a restart is not a new
client with a new address that no node has been told to trust.

The wire formats are lxmf_wire.py; nothing here decodes anything.
"""

from __future__ import annotations

import os
import sys

import RNS
import LXMF

import lxmf_wire


def seed_config(config_dir, peers):
    """Write a config if there is none, because Reticulum's own default is wrong here.

    Left to itself RNS writes a config with an AutoInterface in it and binds a
    UDP port that, on the machine this runs beside, already belongs to the
    daemon. The container then dies with "address already in use", which reads
    as a broken image rather than as two Reticulum instances colliding.

    What is written instead is a standalone instance whose only interfaces are
    the peer entries. Standalone is the accurate word and it was not always the
    word used here: an earlier version claimed this attached to the host's
    running instance as a client, and it never did. RNS names its shared-
    instance socket rns/<instance_name>, defaulting to "default", and a host
    that has set instance_name to anything else is simply not found — the
    container starts its *own* shared instance called rns/default and sits
    there with no interfaces at all, reaching nothing. Every "no path yet" in
    early testing was that, misread as the host having no route to the fleet.

    So a peer is not a fallback for an unusual deployment. Without a peer or a
    mounted host configuration this container can reach nothing whatever.

    To genuinely join the host's instance, give it the host's own configuration
    directory (see the compose files): matching the instance name alone is not
    enough, because the shared-instance RPC is authenticated from the identity
    in that directory and a stranger's digest is rejected.
    """
    if not config_dir:
        return
    path = os.path.join(config_dir, "config")
    if os.path.exists(path):
        return
    os.makedirs(config_dir, exist_ok=True)
    lines = ["[reticulum]",
             "  enable_transport = No",
             "  share_instance = Yes",
             "  panic_on_interface_error = No",
             "",
             "[logging]",
             "  loglevel = 3",
             "",
             "[interfaces]"]
    for i, peer in enumerate(peers):
        host, _, port = peer.partition(":")
        lines += ["  [[peer%d]]" % i,
                  "    type = TCPClientInterface",
                  "    enabled = yes",
                  "    target_host = %s" % host,
                  "    target_port = %s" % (port or "4242")]
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("wrote a starting config to %s" % path, flush=True)


class Client:
    """An LXMF identity that asks nodes questions and hands back their answers.

    `on_message(source_hex, telemetry, text)` is called for every delivery, with
    `telemetry` the raw FIELD_TELEMETRY value if the message carried one — raw,
    because the two readers want different things from it — and `text` the
    message body, which is where a console reply arrives.

    `on_failure(dest_hex, channel, reason, tag)` is called where a request could
    not be sent or was not delivered. Reasons are symbolic — "bad_address",
    "no_path", "delivery_failed" — and `tag` is whatever the caller passed to
    `send`, so a reader that names its requests something of its own gets that
    name back rather than having to reconstruct it.
    """

    def __init__(self, storage, rns_config=None, peers=(), name="retimesh client",
                 on_message=None, on_failure=None):
        self.on_message = on_message
        self.on_failure = on_failure

        seed_config(rns_config, peers)
        RNS.Reticulum(rns_config)

        idpath = os.path.join(storage, "identity")
        if os.path.isfile(idpath):
            identity = RNS.Identity.from_file(idpath)
        else:
            os.makedirs(storage, exist_ok=True)
            identity = RNS.Identity()
            identity.to_file(idpath)

        self.router = LXMF.LXMRouter(identity=identity, storagepath=storage)
        self.local = self.router.register_delivery_identity(identity, display_name=name)
        self.router.register_delivery_callback(self._delivered)
        self.announce()

    @property
    def address(self):
        """This client's LXMF delivery hash, lower-case hex — what to enrol."""
        return RNS.hexrep(self.local.hash, delimit=False)

    def announce(self):
        self.router.announce(self.local.hash)

    def send(self, node_hex, telemetry=False, content="", tag=None):
        """One request to one node. Returns True if it was handed to the router.

        A node whose key this client has never seen cannot be encrypted to, so
        there is nothing to send and asking the network is the whole remedy. It
        is worth doing quietly and often rather than once loudly: a node that
        was asleep at the last attempt answers the next one.
        """
        try:
            dest_hash = bytes.fromhex(node_hex)
        except ValueError:
            print("not a destination hash: %s" % node_hex, file=sys.stderr, flush=True)
            self._failed(node_hex, telemetry, "bad_address", tag)
            return False

        identity = RNS.Identity.recall(dest_hash)
        if identity is None:
            RNS.Transport.request_path(dest_hash)
            self._failed(node_hex, telemetry, "no_path", tag)
            return False

        dest = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                               "lxmf", "delivery")
        fields = {}
        if telemetry:
            # An array of one-entry maps, which is the shape every real client
            # sends and the only one the node's parser is written around.
            fields[lxmf_wire.FIELD_COMMANDS] = [{lxmf_wire.COMMAND_TELEMETRY: None}]

        msg = LXMF.LXMessage(dest, self.local, content, title="",
                             desired_method=LXMF.LXMessage.DIRECT,
                             fields=fields or None)
        msg.register_failed_callback(
            lambda _m, hexed=node_hex, t=telemetry, g=tag:
                self._failed(hexed, t, "delivery_failed", g))
        self.router.handle_outbound(msg)
        return True

    def _failed(self, node_hex, telemetry, reason, tag=None):
        if self.on_failure:
            self.on_failure(node_hex, "telemetry" if telemetry else "console", reason, tag)

    def _delivered(self, message):
        if not self.on_message:
            return
        source = RNS.hexrep(message.source_hash, delimit=False)
        fields = message.fields or {}
        try:
            text = message.content.decode("utf-8", "replace").strip()
        except Exception:
            text = ""
        self.on_message(source, fields.get(lxmf_wire.FIELD_TELEMETRY), text)
