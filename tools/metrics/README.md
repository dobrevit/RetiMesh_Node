# Telemetry exporter — a fleet on a Grafana dashboard

A node does not volunteer telemetry. It answers a request and says nothing
otherwise — the right design where airtime is the scarce thing, and the reason
a Prometheus job pointed at a mesh collects nothing at all. This is the thing
that asks, and the thing Prometheus scrapes.

It joins the mesh as an ordinary LXMF client, polls each node on a timer, and
serves the last answer from each at `/metrics`. `docker compose up` brings it
up with a Prometheus to scrape it and a Grafana with the fleet dashboard
already loaded.

It shares its wire formats and its LXMF client with [the soak
collector](../soak/) — `tools/lxmf_wire.py` and `tools/lxmf_fleet.py` — so what
a reading *is* has one definition rather than one per tool.

## Running it

```sh
cd tools/metrics
cp .env.example .env      # then fill it in; git ignores .env
docker compose up --build
```

Then Grafana on <http://127.0.0.1:3000> (`admin`, and whatever you put in
`GRAFANA_PASSWORD`), with **RetiMesh fleet** in the RetiMesh folder.

`.env` is where the deployment's own facts go — which nodes exist and how to
reach them. They are not in the compose file on purpose: a compose file
carrying them is a compose file that commits them.

| variable | |
|---|---|
| `RETIMESH_NODES` | LXMF delivery hashes, comma-separated, each optionally `=name`. A node reports its own under `STATUS`, the `lxmf_address=` line |
| `RETIMESH_COMMANDS` | console lines to poll as well — `STACKS,POWER` usually. Needs enrolment; see below |
| `RETIMESH_PEERS` | `host:port` Reticulum transports to dial directly. **Required in the standalone shape the compose ships** — without one the exporter has no interfaces and reaches nothing |
| `RETIMESH_LISTEN` | where `/metrics` is served. Change it and change the target in `prometheus.yml` to match — Prometheus does not read `.env` |

The exporter prints its address at startup:

```
exporter address: 5315940e088ac37220be1e7b2dd207d3
enrol that on each node to allow console commands (telemetry needs no enrolment)
```

Telemetry needs no enrolment. The console channel does — remote administration
is off by default, and an empty administrator list is off however the switch
reads:

```
SET maintenance.rns_admins <that address>
SET maintenance.rns_admin on
```

Both apply live. Without it every console request is answered `RM ERR ADMIN
403`, which the exporter counts as `retimesh_node_console_errors_total` — the
dashboard has a panel for it, because otherwise a missing enrolment looks
exactly like a node that never answers.

It runs perfectly well outside the container, too:

```sh
pip install rns lxmf
python tools/metrics/exporter.py --node <hash>=hilltop --peer 10.0.0.21:4242 \
    --storage /tmp/rns --rns-config /tmp/cfg --listen :9812
```

## What it collects

**Telemetry**, over LXMF `FIELD_COMMANDS` (0x09) command `0x01`: clock,
battery, position, signal, processor, RAM and storage. Open to any sender,
subject to the node's ten-second per-sender cooldown.

**Console replies**, by sending an ordinary message whose text is a console
line. It reaches the same parser as the cable, so `STACKS` gives per-task stack
headroom — the measurement a soak exists to take — and `STATUS` gives the
restart count and reason, the previous run's fault counters and the heap.

A console reply over LXMF is capped at **200 bytes**, so that the whole message
fits in one packet. `STACKS` is built around that. `STATUS` is not: only its
first three or so lines arrive, and the node appends `(reply truncated; ask for
less at once)`. Those first lines happen to be the valuable ones, and
everything mapped below them is simply absent — a missing series rather than a
wrong one.

Firmware that has the `POWER` command answers the cell and the profile there
instead, in one short line built to travel. The mapping covers both, and
whichever answered most recently wins where they overlap, so the same
configuration works either side of that change. `HELP` on the console lists
what a given build knows.

**Telemetry that arrives unasked.** Sideband can be told to push its own, and a
node enrolled against this address may too. Those nodes appear with
`retimesh_node_polled 0`. Pass `--only-configured` to ignore them, which is
worth doing on a public mesh: anyone can send a readings map, and a series that
appears because a passer-by pressed a button is one nobody can explain later.

## The three rules the metrics follow

Everything awkward about exporting mesh telemetry comes from one gap: **a
scrape happens every fifteen seconds and a LoRa node answers every five
minutes, if it answers.**

**A node that has gone quiet stops publishing readings.** Holding the last
battery percentage would draw a confident flat line for a node that fell off a
hill three days ago, and a flat line reads as a working sensor. After
`--max-age` (three poll intervals by default) the readings are withheld and
Prometheus marks the series stale. What stays is `retimesh_node_up` at 0, the
last-reply timestamp, the counters and `retimesh_node_info` — the node is still
known, and "known and silent" has to look like something other than an absence.
The dashboard's node table shows exactly that: the row stays, the readings
empty. **Alert on `retimesh_node_up`, not on a reading.**

**A reading the board cannot take is absent, not zero.** A board that cannot
see its charger sends nil rather than false, because "not charging" sends
somebody looking for a fault in a working cable — so
`retimesh_node_battery_charging` has *no series at all* for that board, which
is not the same as 0 and must not be graphed as it. Likewise a node whose RTC
domain dropped reports `prev_alloc_failures=unknown` rather than 0, and an
exporter that published a 0 there would hide a repeating panic. It publishes
nothing instead.

**Strings are not values.** Firmware version, board, power profile and reset
reason are labels on `_info` metrics carrying a constant 1. Join them on:

```promql
retimesh_node_battery_percent * on(node) group_left(name, board) retimesh_node_info
```

Hanging them on every metric would give every series a new identity the moment
a node was upgraded, breaking the graph meant to show the upgrade. A node with
no configured name is labelled by the first eight characters of its address, so
a legend reads as something rather than as a gap.

## The metrics

`node` is the LXMF delivery hash on every one of them.

| | |
|---|---|
| `retimesh_node_up` | 1 when it answered inside the freshness window, 0 when known and silent |
| `retimesh_node_info` | `name`, `board`, `firmware`, `information` — the join target |
| `retimesh_node_polled` | 1 for a node this exporter asks, 0 for one that only volunteers |
| `retimesh_node_last_reply_timestamp_seconds`, `..._first_seen_...` | when it was last and first heard |
| `retimesh_node_clock_timestamp_seconds` | the node's own clock. `time() - this` is the error; a node with no GNSS fix and no RTC stamps 1970, which reads here as about fifty-six years |
| `retimesh_node_battery_percent`, `_charging`, `_temperature_celsius` | charging is absent where the board cannot see its charger |
| `retimesh_node_position_*` | latitude, longitude, altitude, speed, bearing, accuracy, and when the fix was taken |
| `retimesh_node_rssi_dbm`, `_snr_db`, `_link_quality_percent` | how the node heard *this exporter's request* — a property of the path, not of the node |
| `retimesh_node_ram_capacity_bytes`, `_used_bytes` | **internal** memory, not the total including PSRAM |
| `retimesh_node_storage_*{part}` | `flash` never moves; `card` fills |
| `retimesh_node_cpu_clock_hertz` | |
| `retimesh_node_requests_total{channel}`, `_replies_total{channel}` | silence is a result: a node that was asked and did not answer is a different fact from one never asked |
| `retimesh_node_request_failures_total{reason}` | `no_path` or `delivery_failed` |
| `retimesh_node_console_errors_total{command,code}` | a steady `403` is a missing enrolment |
| `retimesh_exporter_*` | this process: address, library versions, scrapes, poll rounds |

From the console, and absent without enrolment:

| | |
|---|---|
| `retimesh_node_task_stack_headroom_bytes{task}`, `_stack_tightest_headroom_bytes{task}` | from `STACKS` |
| `retimesh_node_battery_volts`, `_battery_reading_stale`, `retimesh_node_battery_sense_info{state}` | the cell, from `STATUS` or `POWER`. `state` is `present`, `stale` (the converter stopped answering) or `not-seen` (none fitted) — the last two both report no battery and send you to opposite ends of the board |
| `retimesh_node_power_info{profile,pmu,wifi_ps}` | from `STATUS` or `POWER` |
| `retimesh_node_uptime_seconds`, `_boots_total`, `retimesh_node_boot_info{reset}` | from `STATUS` or `POWER` |
| `retimesh_node_alloc_failures`, `_contained_faults` | **this** run, zeroed by every restart |
| `retimesh_node_previous_run_seconds`, `_alloc_failures`, `_contained_faults` | the run before, which is the only surviving explanation of the restart you are investigating |
| `retimesh_node_heap_*`, `_dram_*`, `_psram_free_bytes` | from `STATUS` |
| `retimesh_node_lora_rx_packets_total`, `_tx_packets_total`, `_cad_timeouts_total`, `_cad_arm_errors_total`, `_radio_online`, `retimesh_node_radio_info{model}` | from `STATUS` |

A console key that is not in the table in `state.py` is ignored rather than
guessed at. Automatic naming would produce metrics without units, and a key's
meaning is not in its name: `heap_free` is internal memory and `dram_free` is
the byte-addressable part of the same, and only one of them says whether
another task can be placed.

## Two things worth knowing before trusting the numbers

**Stack headroom is a high-water mark**, the least a task has ever had left. It
only falls, so the last reading of a long run is the one that matters — and it
is still *worst seen*, not *worst possible*. A stack cut to its observed peak is
a crash waiting for a path nothing has taken yet. Leave margin.

**Telemetry sends the instantaneous free figure, not the low-water mark.** The
node tracks its minimum and does not send it, so an exporter sampling every few
minutes will miss dips. On one bench board the reported free internal RAM sat
around 22 KB while the minimum for that same run was 6 KB. Treat
`retimesh_node_ram_*` as the trend and the console's `heap_min`/`dram_min` as
the measurement.

## How it reaches anything

Two shapes, and the compose file ships the first — the same two as the soak
collector, which documents them at length.

**Standalone**: its only interfaces are the `RETIMESH_PEERS` entries, each
dialled straight to a node's Reticulum transport. **With no peers it reaches
nothing at all**, and the symptom is a stream of `no_path` failures, which reads
like a routing problem and is in fact an empty interface list.

**Joined to the host's instance**: uncomment the `~/.reticulum` mount and the
`user:` line in the compose file and drop `RETIMESH_PEERS`. It then inherits
every interface the host has, LoRa included. That needs the host's own
*configuration directory*, not merely a matching instance name: the
shared-instance RPC is authenticated from the identity in it, and a stranger's
digest is rejected.

All three services run on the **host's network**, because that second shape
needs it — a shared instance's socket is abstract and therefore lives in a
network namespace, and a container on a bridge cannot see it however the ports
are published. The cost is that a service on `0.0.0.0` is a service on the LAN,
so all three default to loopback. None of them speaks TLS; put something in
front if you want them reachable.

## Restarting it

Readings are held in memory only. A restart is a gap of up to one poll
interval, visible as one; Prometheus keeps everything it already scraped. What
must survive is `data/exporter/` — the identity in it is what the nodes are
enrolled against, and losing it means an exporter with a new address that no
node will take a console command from.

The container runs as root, so `data/` ends up root-owned. That is also why the
`user:` line matters if you mount `~/.reticulum`: root-owned files appearing
there will break the daemon that owns it.

## Versions

The image pins `rns` to the version the **host** instance runs, not the newest
published, and `prom/prometheus` and `grafana/grafana` to exact tags. A fleet
watched for months should not be reading a different protocol in March than it
was in January. Check the host before raising the first:

```sh
python3 -c "import RNS; print(RNS.__version__)"
```

Note that this host runs Reticulum from an editable fork rather than from pip.
If that fork carries changes to the wire protocol, a stock `rns` in the
container will not match it and the pin is not enough — build the image against
the fork instead.
