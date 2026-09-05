# Soak collector

A node does not volunteer telemetry. It answers a request and says nothing
otherwise — the right design where airtime is the scarce thing, and the reason
a soak test with nobody asking produces no data at all. This is the thing that
asks.

It runs as a container beside the Reticulum instance the host already has, polls
each node on a timer, and appends every answer to newline-delimited JSON.

## What it collects

**Telemetry**, over LXMF `FIELD_COMMANDS` (0x09) command `0x01`. Open to any
sender, subject to the node's ten-second per-sender cooldown — no enrolment
needed. Gives the clock, battery, position, signal, processor, storage, and RAM.

The RAM figure is the node's **internal** memory, not the total including PSRAM.
That distinction is the whole point of collecting it: on a board with 8 MB of
PSRAM the total looks healthy right up until the node dies of the kind it
actually needs.

**Console replies**, by sending an ordinary message whose text is a console
line. It reaches the same parser as the cable, so anything the console answers
works — `STACKS` is the one worth a soak, since per-task stack headroom only
ever falls and days of real traffic are worth more than any bench session.

This channel **requires enrolment**: remote administration is off by default and
an empty administrator list is off however the switch reads. The collector
prints its address at startup; that is what to enrol.

## Running it

```sh
cd tools/soak
mkdir -p data
# put each node's LXMF delivery hash in docker-compose.yml, then:
docker compose up --build
```

First start prints the address to enrol:

```
collector address: <a1b2c3…>
enrol that on each node to allow console commands (telemetry needs no enrolment)
```

Enrol it on each node — over the console, or over the cable:

```
SET rns.admins <that address>
SET rns.admin_enabled on
```

Telemetry works without any of that; only the console channel needs it.

## Reading what it wrote

One JSON object per line, appended. A run that is interrupted keeps what it had
and a later run continues the same file.

Requests are recorded as well as answers — `{"sent": true}` going out, then a
`kind: "reply"` row coming back. Silence is a result during a soak: a node that
was asked and did not answer is a different fact from one that was never asked,
and a file containing only replies cannot tell them apart three days later.

```sh
# how internal RAM has moved, per node
jq -r 'select(.telemetry.ram) | [.at, .node, .telemetry.ram.free] | @tsv' data/soak.jsonl

# the tightest stack each node has reported
jq -r 'select(.lines) | .lines[] | select(startswith("RM STACKS tightest"))' data/soak.jsonl
```

## Two things worth knowing before trusting the numbers

**Stack headroom is a high-water mark**, the least a task has ever had left. It
only falls, so the last reading of a long run is the one that matters — and it
is still *worst seen*, not *worst possible*. A stack cut to its observed peak is
a crash waiting for a path nothing has taken yet. Leave margin.

**Telemetry sends the instantaneous free figure, not the low-water mark.** The
node tracks its minimum and does not send it, so a collector sampling every few
minutes will miss dips. On one bench board the reported free internal RAM sat
around 22 KB while the minimum for that same run was 6 KB. Treat telemetry as
the trend and the console as the measurement.

## Versions

The image pins `rns` to the version the **host** instance runs, not the newest
published. The collector joins that instance, and a client speaking a slightly
different protocol to the routing table it shares is worse than an old one.
Check before raising it:

```sh
python3 -c "import RNS; print(RNS.__version__)"
```

Note that this host runs Reticulum from an editable fork rather than from pip.
If that fork carries changes to the wire protocol, a stock `rns` in the
container will not match it and the pin above is not enough — build the image
against the fork instead.

## Why host networking

The collector joins the Reticulum instance this machine already runs rather than
starting a second one — two instances on one host are two routing tables that
disagree. On Linux that instance is reachable at 127.0.0.1 only from the host's
own namespace, so the container shares it. A bridged container would instead
need the host's `rnsd` listening on a routable address, which is a change to the
host rather than to this.
