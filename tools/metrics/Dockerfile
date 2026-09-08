# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
#
# The telemetry exporter: an RNS client that asks a fleet how it is and serves
# the last answers to Prometheus. Slim rather than alpine — RNS pulls in
# cryptography, and the wheels for it exist for glibc without a compiler.
FROM python:3.12-slim

# Pinned so a fleet watched for months is not reading a different protocol in
# March than it was in January. Raise them deliberately.
#
# RNS is pinned to the version the host instance runs — 1.5.0 here — rather
# than to the newest. Where this joins that instance, a client speaking a
# slightly different protocol to the routing table it shares is worse than an
# old one. Check the host before raising it:
#
#     python3 -c "import RNS; print(RNS.__version__)"
#
# Nothing else is installed. The Prometheus exposition format is written out by
# hand (exposition.py) because this exporter's whole job is to leave series
# *out*, and the client library is built around gauges that always have a value.
RUN pip install --no-cache-dir rns==1.5.0 lxmf==1.1.1

WORKDIR /app
# Built from tools/, because the wire formats and the LXMF client are shared
# with the soak collector (tools/soak/). One definition of what a reading is.
COPY lxmf_wire.py lxmf_fleet.py /app/
COPY metrics/exposition.py metrics/state.py metrics/exporter.py /app/

# The identity lives here, and it is what the nodes are enrolled against: a
# restart that loses it is an exporter with a new address that no node will
# take a console command from.
VOLUME ["/data"]

EXPOSE 9812
ENTRYPOINT ["python", "-u", "/app/exporter.py"]
