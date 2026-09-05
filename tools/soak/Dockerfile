# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd — part of RetiMesh Node, see LICENSE.
#
# The soak collector: an RNS client that asks a fleet how it is and keeps the
# answers. Slim rather than alpine — RNS pulls in cryptography, and the wheels
# for it exist for glibc without a compiler in the image.
FROM python:3.12-slim

# Pinned so a soak that runs for days is not reading a different protocol on
# day three than it was on day one. Raise them deliberately, between runs.
#
# RNS is pinned to the version the host instance runs — 1.5.0 here — rather than
# to the newest. The collector joins that instance, and the one thing worse than
# an old client is a client speaking a slightly different protocol to the
# routing table it shares. Check the host before raising it:
#
#     python3 -c "import RNS; print(RNS.__version__)"
RUN pip install --no-cache-dir rns==1.5.0 lxmf==1.1.1

WORKDIR /app
COPY collector.py /app/collector.py

# Identities, LXMF state and the output all live here, so a restart of the
# container is not a new collector with a new address the nodes have never
# been told to trust.
VOLUME ["/data"]

ENTRYPOINT ["python", "-u", "/app/collector.py"]
