# The serial byte-stream link

A UART that carries HDLC frames and knows nothing about what is inside them.
It exists so that the Reticulum serial interface and the remote-radio link can
both sit on one wire driver instead of each writing a UART of their own.

It is **not enabled on any board**. That is deliberate, and the rest of this
page is mostly about why, and what enabling it costs.

## What it is

`src/net/UartLink.h` owns one UART: its receive task, a bounded transmit queue,
and counters. It hands whole frames up through a sink and takes whole frames
down through `send()`. It has no opinion about Reticulum, packets, addresses or
anything else above the wire.

Three things it deliberately does not do itself, because they already exist and
are already tested:

| What | Where | Pinned by |
|---|---|---|
| Framing and resynchronisation | `src/net/HDLC.h` | `test_hdlc`, ten corruption cases |
| The queue's bound and counters | `src/net/UartTxQueue.h` | `test_uart_tx_queue` |
| Whether a baud is qualified for this board | `LocalLink::pppBaudUsable` | `test_local_link` |
| Whether a UART already belongs to something | `src/net/UartPortPolicy.h` | `test_uart_port_policy` |

## Why no board has it on

Because which UART is free is a per-board fact, and on this hardware most of
them are already spoken for:

- **The bridge UART** (`BOARD_UART_INSTANCE`, usually UART0) belongs to the
  console, and to PPP when that is switched on. They take turns through an
  arbiter — `src/net/PppArbiter.h` — because a console reply arriving in the
  middle of a PPP frame corrupts it.
- **UART1** belongs to the GNSS receiver on every board that has one. The
  number has one home, `Gps::kUartInstance`, so a board that moves the receiver
  moves this check with it.
- **Whatever is left** comes out on pins that depend on the board, and on most
  of them those pins are not broken out anywhere a wire can reach.

The last point is the one that keeps this switched off. The firmware can see
two of the three claimants and `begin()` refuses a port that collides with
either, but it cannot see whether a pin goes to a header, to a test pad, or
nowhere at all. Naming a port here without having looked at the board would
claim a link that does not exist.

## Enabling it on a board

Three defines in the board's header, and a bench session before them:

```c
#define BOARD_SERIAL_UART    2      // a port nothing else owns
#define PIN_SERIAL_LINK_RX   16     // pins that actually come out somewhere
#define PIN_SERIAL_LINK_TX   17
```

`HAS_SERIAL_LINK` follows from all three being present, and everything in the
driver is behind it — so a board that says nothing compiles none of it and pays
neither flash nor DRAM.

Check before you write them:

1. The port is not `BOARD_UART_INSTANCE` and not the GNSS receiver's.
   `begin()` refuses both — the rule is `UartPortPolicy.h` and it is tested —
   but finding out at boot is worse than finding out now.
2. The pins reach somewhere a wire can be attached, and are not already a chip
   select, an enable line or a strapping pin. `docs/hardware.md` has the pin map
   for each board; the board header is the authority.
3. `python3 tools/board_facts.py <env>` prints what the catalogue thinks the
   board has, which is a faster way to check for a GNSS receiver than reading
   the header.

There is a compile-only env, `t3s3-serial`, which turns the driver on with
arbitrary pins so that it builds. It is not a shipping target and its pins
describe nothing — it exists because code that is never compiled rots faster
than code that is merely unused.

## What it costs when it is on

About 650 bytes are held from static initialisation whether or not `begin()`
is ever called — `HardwareSerial`'s constructor creates a mutex, and the
deframer's 500-byte buffer and the queue's fields are `.bss`. Everything else
arrives with `begin()` and is given back by `end()`:

| | Size | Notes |
|---|---|---|
| Static, always | ~650 B | the port's mutex, the deframer's buffer, the queue |
| Transmit arena | `SERIAL_LINK_TX_BYTES`, 4 × `RNS_MTU` = 2000 B | heap, freed on `end()` |
| Frame table | `SERIAL_LINK_TX_FRAMES` × 8 B = 64 B | heap, freed on `end()` |
| Receive ring | `SERIAL_LINK_RX_RING`, 2048 B | the UART driver's, released with the port |
| Reader task | `SERIAL_LINK_TASK_STACK`, 3072 B | core 0, priority 2 |

That is about 7.2 KB while the link is up, plus the 650 bytes above that a
board carries for enabling it at all. On a board with ten kilobytes of
byte-addressable DRAM to spare that is not a background cost to carry for a
link nobody is using, which is the whole reason `begin()`/`end()` allocate and
free rather than holding statics.

## The baud ladder is borrowed, and that matters

`begin()` accepts only speeds the board has been qualified at, using the same
rule PPP is held to. But that ladder and its ceiling (`tested_max_baud`) are
facts about the **bridge** UART — the CP2102 or CH9102 and the cable behind it
— and this link is a different port on different pins with no bridge chip on
them. The ceiling is borrowed deliberately, as the conservative placeholder,
because no bench has qualified this wire.

The visible consequence: `tested_max_baud` is 115200 on every board today, so
"sustained transfer at each supported baud" means one speed until the registry
gains a field for this link's own qualification. Raising it is a bench job
followed by a `boards.json` change, not a code change.

## The transmit queue

Bounded in two ways at once — a frame count and a byte total — because a link
is as likely to be held up by many small frames as by a few large ones, and a
bound that counted only one of them would be no bound in the other direction.

**A full queue refuses the new frame.** It does not evict the oldest to make
room. The frames already queued are the ones a peer is waiting on, and a sender
that discards them to admit newer traffic turns a slow link into a lossy one at
exactly the moment the far end is least able to tell the difference. `send()`
returning false is backpressure the caller can see, and the drop counter is how
it sees it afterwards.

## Counters

`UartLink::counters()` reports frames and bytes in both directions, the queue's
depth and its high-water mark, drops, and frames too long for the deframer.

Two counters the roadmap asks for are **absent rather than zero**:
resynchronisations, and receive-ring overruns. Neither is observable from here
today — `HDLC::Deframer` has no checksum, so it cannot tell a truncated frame
from a complete one, and `HardwareSerial` does not surface the driver's ring
overflow. Both need a change to a component with tests of its own. A field that
reads zero for ever is worse than a missing one, because a surface prints it as
a measurement.

## What has not been proved

The link has never run on hardware. What is proved today is the queue — that it
is counted, that it cannot grow without bound, and that frames come back byte
for byte across the arena's wrap — and that the driver compiles.

Unproven, and needing two boards and a wire:

- sustained transfer at each qualified baud with no loss;
- pulling the wire mid-frame and reconnecting, resuming on the next valid
  frame;
- interrupt load at the top baud against radio timing, which the roadmap flags
  as worth measuring before promising 921600.

There is also nothing on the far end yet: the interface that would use this is
`SerialRnsInterface`, which is a separate piece of work.
