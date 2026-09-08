#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dobrev IT Ltd
#
# This file is part of RetiMesh Node. See LICENSE.

"""Prove — or disprove — that duty-cycled LoRa receive drops nothing.

`radio.rx_duty_cycle` puts an SX1262 to sleep between preamble samples
(`startReceiveDutyCycleAuto`, sized on the network's 18-symbol preamble floor).
It ships defaulted off and no profile may arm it until there is bench evidence
that a sleeping receiver hears everything a continuously-listening one does.
This is the harness that collects that evidence.

What it does, per channel under test:

  * transmits N frames from an RNode through Reticulum, each carrying its own
    sequence number, paced so the sender stays inside the sub-band's hourly
    transmit allowance;
  * reads the node's LoRa receive counter over its maintenance console before
    and after each frame, so a loss is attributed to *that* sequence number
    rather than merely subtracted from a total at the end;
  * runs the same frames with `radio.rx_duty_cycle` off and on, interleaved in
    blocks, so the comparison is against the same rig minutes apart rather than
    against an assumption about what a perfect link does.

The comparison is the whole point, and it is why the criterion this replaces
could never have been met. "Zero missed packets across 10 000 frames" asks an
RF link for a 0.00 % raw loss rate; no over-the-air link delivers that, so a
run that failed would not distinguish a mis-sized sleep window from a passing
car. What can be established is that the duty-cycled arm loses no more than the
continuous arm, to a stated confidence — see `--frames` and `verdict()` below,
and `roadmap/power/07-rx-duty-cycle-hil.md` for the arithmetic that sets N.

Three things this cannot see, said out loud because a harness that hides them
is worse than none:

  * The node reports a *count*, not the frames themselves. Sequence identity
    comes from polling the counter around each individual transmission, which
    is exact but costs a console round trip per frame. A firmware that exposed
    the last few received frames' identities would let this run in blocks.
  * A frame the node was transmitting through cannot be received, and the node
    announces on its own schedule. Every window records the node's transmit
    counter too, and a window in which it moved is excluded rather than scored
    (`--keep-announces` leaves the schedule alone; by default the announce
    interval is set to 0 for the run and restored afterwards).
  * Another radio on the channel would inflate the receive counter and read as
    a delivery. A pre-flight listens for a stated quiet period and refuses to
    start on a channel that is not silent.

Usage:

    tools/hil_frames.py --dry-run                       what it would do, and how long
    tools/hil_frames.py --dry-run --plan original       what the 10k criterion would cost
    tools/hil_frames.py --node /dev/ttyACM3 --rnode-port /dev/ttyACM1 --out run.jsonl
    tools/hil_frames.py --node /dev/ttyACM3 --out run.jsonl --resume

Exit code is the number of channels that did not pass, 0 for a clean sweep —
the same contract every other HIL script here keeps.
"""

import argparse
import json
import math
import os
import random
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "retimesh-flash"))

from hilreport import Reporter                                    # noqa: E402


# ===========================================================================
#  The channel arithmetic, mirrored from the firmware
# ===========================================================================
#
# Everything below reproduces `src/radio/Airtime.cpp` and `src/Config.h`
# rather than deriving an equivalent. That is a deliberate second copy of a
# rule, which this project otherwise forbids, and it is justified only because
# the first copy runs on a microcontroller and cannot be called from here. The
# obligation that comes with it is discharged in `tools/tests/test_hil_frames.py`:
# those tests read the C++ sources and fail if any constant here stops matching
# the firmware's own. A change to the band table, the preamble floor or the
# RadioLib mirror breaks a host test rather than quietly producing a bench run
# that paced itself against a limit the node no longer obeys.

# src/Config.h — the shipped channel and the network's preamble floor.
RF_FREQ_MHZ = 869.525
RF_BW_KHZ = 125.0
RF_SF = 8
RF_CR = 5
RF_PREAMBLE_SYMS = 18

# src/radio/Airtime.h — RadioLib 7.7.1's arithmetic, as the firmware mirrors it.
RX_DC_MIN_SYMBOLS_SF7 = 8
RX_DC_MIN_SYMBOLS_SF6 = 12
RX_DC_TRANSITION_US = 1016
RX_DC_COMPENSATION_US = 1000
RX_DC_PERIOD_RAW_MAX = 0x00FFFFFF
RX_DC_TCXO_DELAY_US = 5000

# src/radio/Airtime.h — the safety margin the node holds back from the legal
# ceiling. The sender must respect the same allowance the node would, because
# the allowance belongs to the channel and not to whichever radio is using it.
DUTY_MARGIN_PCT = 5

# src/radio/Airtime.cpp — the EU 863-870 MHz SRD sub-bands, allowances in basis
# points. The unallocated gaps are listed, as they are in the firmware: a
# channel there is not "unlimited", it is not permitted at all, and it carries
# the strictest figure in the plan.
EU_BANDS = (
    (863.00, 865.00, 10, "863-865 (0.1 %)"),
    (865.00, 868.00, 100, "865-868 (1 %)"),
    (868.00, 868.60, 100, "868-868.6 (1 %)"),
    (868.60, 868.70, 10, "868.6-868.7 (not allocated)"),
    (868.70, 869.20, 10, "868.7-869.2 (0.1 %)"),
    (869.20, 869.40, 10, "869.2-869.4 (not allocated)"),
    (869.40, 869.65, 1000, "869.4-869.65 (10 %)"),
    (869.65, 869.70, 10, "869.65-869.7 (not allocated)"),
    (869.70, 870.00, 100, "869.7-870 (1 %)"),
)

# src/radio/RadioCaps.cpp — every bandwidth an SX1262 offers below 1 GHz.
SX126X_BANDWIDTHS = (7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125.0, 250.0, 500.0)

# RNS.Packet.pack(): flags + hops + a 16-byte truncated destination hash +
# context, then the payload. A PLAIN destination does not encrypt, so what goes
# on the air is exactly this. RNode_Firmware.ino:730 prepends one framing byte
# of its own to every LoRa packet it transmits.
RNS_HEADER_BYTES = 2 + 16 + 1
RNODE_HEADER_BYTES = 1


def symbol_time_ms(sf: int, bw_khz: float) -> float:
    """2^SF / BW(kHz), in milliseconds. Airtime::symbolTimeMs()."""
    return float(1 << sf) / bw_khz


def time_on_air_ms(sf: int, bw_khz: float, payload_bytes: int, cr: int = RF_CR,
                   preamble_syms: int = RF_PREAMBLE_SYMS, crc_on: bool = True,
                   implicit_header: bool = False) -> float:
    """LoRa time on air, per the Semtech modem datasheets. Airtime::timeOnAirMs().

    `payload_bytes` is what the modem transmits — for this harness that is the
    RNS packet plus the RNode's framing byte, not the sequence number alone.
    """
    t_sym = symbol_time_ms(sf, bw_khz)
    de = 1 if t_sym > 16.0 else 0            # low-data-rate optimisation, mandatory over 16 ms
    ih = 1 if implicit_header else 0
    crc = 1 if crc_on else 0
    num = 8.0 * payload_bytes - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih
    den = 4.0 * (sf - 2 * de)
    payload_symbols = 8.0 + max(math.ceil(num / den) * ((cr - 4) + 4), 0)
    return (preamble_syms + 4.25) * t_sym + payload_symbols * t_sym


def rx_duty_cycle_min_symbols(sf: int) -> int:
    """Airtime::rxDutyCycleMinSymbols() — RadioLib's per-SF sampling floor."""
    return RX_DC_MIN_SYMBOLS_SF6 if sf <= 6 else RX_DC_MIN_SYMBOLS_SF7


def rx_duty_cycle_sleep_us(sf: int, bw_khz: float,
                           preamble_syms: int = RF_PREAMBLE_SYMS,
                           min_symbols: int = 0) -> int:
    """Airtime::rxDutyCycleSleepUs(): microseconds asleep per cycle, 0 for never.

    The driver's truncating symbol expression is reproduced exactly, including
    the integer truncation, because the threshold below is compared against the
    truncated figure and a nicer expression moves the boundary.
    """
    sf = min(max(sf, 5), 12)
    if not bw_khz > 0.0:
        return 0
    if min_symbols == 0:
        min_symbols = rx_duty_cycle_min_symbols(sf)
    if 2 * min_symbols >= preamble_syms:
        return 0
    sleep_symbols = preamble_syms - 2 * min_symbols
    symbol_us = int((10000 << sf) / (10.0 * bw_khz))
    return symbol_us * sleep_symbols


def rx_duty_cycle_engages(sleep_us: int, tcxo_delay_us: int = RX_DC_TCXO_DELAY_US) -> bool:
    """Airtime::rxDutyCycleEngages(): will the driver actually take that sleep?

    False covers both of RadioLib's refusals — too short, so it silently arms a
    plain continuous receive, and too long, so it arms nothing at all. A channel
    that answers false is one where this harness is measuring the *fallback*,
    which is a real thing to measure and a different claim from the one the
    feature makes.
    """
    if sleep_us < tcxo_delay_us + RX_DC_TRANSITION_US:
        return False
    period = sleep_us - (tcxo_delay_us + RX_DC_COMPENSATION_US)
    raw = (period * 8) // 125
    return raw != 0 and raw <= RX_DC_PERIOD_RAW_MAX


def rx_duty_cycle_wake_us(sf: int, bw_khz: float) -> int:
    """PhysicalLayer::calculateRxDutyCycle's wake window, for the cycle period.

    Not used to decide anything — the firmware never needs it — but the cycle
    period is what a frame's arrival phase is uniform over, and the pacer
    jitters by one full cycle so that consecutive frames cannot sample the same
    phase. Reported in the plan so an operator can see the geometry being tested.
    """
    s = int((10000 << sf) / (10.0 * bw_khz))
    return max((17 * s + 1000) // 2, 9 * s)


def band_for(freq_mhz: float, bw_khz: float = 0.0):
    """Airtime::bandFor(): the strictest EU sub-band the channel touches.

    A channel is not a point. Energy lands across the whole bandwidth, so a
    carrier sitting on a sub-band boundary is held to the meaner side of it —
    which is exactly what catches a 500 kHz channel at 869.525 MHz, where the
    skirts reach into two unallocated ranges and the allowance collapses from
    10 % to 0.1 %.
    """
    half = (bw_khz if bw_khz > 0.0 else 0.0) / 2000.0
    lo, hi = freq_mhz - half, freq_mhz + half
    strictest = None
    for low, high, bp, name in EU_BANDS:
        overlaps = (hi > low and lo < high) or (half == 0.0 and low <= freq_mhz <= high)
        if not overlaps:
            continue
        if strictest is None or bp < strictest[2]:
            strictest = (low, high, bp, name)
    return strictest


def effective_basis_points(freq_mhz: float, bw_khz: float, manual_pct: int = 0) -> int:
    """Airtime::effectiveBasisPoints(): the allowance the node holds itself to.

    0 means no limit is known, which happens only outside the plan with no
    manual cap — and this harness refuses to pace against "no limit" rather
    than transmitting flat out on a channel nobody has a rule for.
    """
    band = band_for(freq_mhz, bw_khz)
    manual = manual_pct * 100
    if band is None:
        return manual
    allowed = band[2] * (100 - DUTY_MARGIN_PCT) // 100
    if allowed == 0:
        allowed = 1
    if 0 < manual < allowed:
        return manual
    return allowed


def on_air_bytes(payload_bytes: int) -> int:
    """What the modem transmits for a payload of `payload_bytes` through RNS."""
    return RNS_HEADER_BYTES + payload_bytes + RNODE_HEADER_BYTES


# ===========================================================================
#  The plan
# ===========================================================================

def default_rns_python() -> str:
    """The interpreter to run the sending child under.

    RNS lives in the Reticulum virtualenv on this bench and not in the system
    Python that has pyserial, so the two halves of this harness genuinely need
    different interpreters. The venv beside the checkouts is tried first and
    the current interpreter is the fallback; `--rns-python` overrides both, and
    a wrong guess fails loudly in the child's first line rather than silently.
    """
    candidate = Path(__file__).resolve().parents[2] / ".venv" / "bin" / "python"
    return str(candidate) if candidate.exists() else sys.executable


FRAME_MAGIC = b"RMHF"


def frame_payload(seq: int) -> bytes:
    """The RNS payload for one frame: a magic and a 32-bit sequence number.

    The magic is not needed by the counter arithmetic — the harness knows which
    sequence it sent in which window. It is there so that a serial capture of
    the node, or a promiscuous RNode watching the channel, can corroborate the
    run independently, which is the only cross-check available while the console
    reports counts and not frames.
    """
    return FRAME_MAGIC + struct.pack("!I", seq)


PAYLOAD_BYTES = len(frame_payload(0))


class Point:
    """One channel under test, and how many frames it gets.

    `engages` is derived, never declared: it is the firmware's own predicate for
    this channel. A point where it is false is not a mistake in the plan — it is
    the shipped SF8/125 kHz case, where the driver falls back to a continuous
    receive, and proving the fallback loses nothing is part of the claim.
    """

    def __init__(self, sf: int, bw_khz: float, frames: int, note: str = ""):
        self.sf = sf
        self.bw_khz = bw_khz
        self.frames = frames
        self.note = note

    @property
    def key(self) -> str:
        return "sf%d_bw%g" % (self.sf, self.bw_khz)

    @property
    def sleep_us(self) -> int:
        return rx_duty_cycle_sleep_us(self.sf, self.bw_khz)

    @property
    def engages(self) -> bool:
        return rx_duty_cycle_engages(self.sleep_us)

    @property
    def cycle_ms(self) -> float:
        return (rx_duty_cycle_wake_us(self.sf, self.bw_khz) + self.sleep_us) / 1000.0

    def toa_ms(self, payload_bytes: int = PAYLOAD_BYTES, cr: int = RF_CR) -> float:
        return time_on_air_ms(self.sf, self.bw_khz, on_air_bytes(payload_bytes), cr)

    def gap_s(self, freq_mhz: float, payload_bytes: int = PAYLOAD_BYTES,
              cr: int = RF_CR, poll_ms: float = 150.0, settle_ms: float = 400.0,
              manual_pct: int = 0) -> float:
        """Seconds between the start of one frame and the start of the next.

        Two floors, and the larger wins. The regulatory one is the sub-band's
        hourly allowance applied instantaneously, which is the only pacing that
        keeps a run of thousands of frames legal without a rolling accumulator.
        The mechanical one is what a window actually costs: the frame's own
        airtime, the settle the node needs to service the interrupt, and the two
        console round trips that read the counter either side of it.
        """
        bp = effective_basis_points(freq_mhz, self.bw_khz, manual_pct)
        if bp <= 0:
            raise ValueError(
                "no transmit allowance is known for %.3f MHz at %g kHz — set "
                "--duty-cycle-pct to state one rather than transmitting unpaced"
                % (freq_mhz, self.bw_khz))
        toa = self.toa_ms(payload_bytes, cr)
        regulatory = (toa / 1000.0) / (bp / 10000.0)
        mechanical = (toa + settle_ms + 2 * poll_ms) / 1000.0
        return max(regulatory, mechanical)


def build_plan(name: str, frames: int, fallback_frames: int) -> list:
    """The channels a run covers, and why each one is in it.

    `claim` is the runnable replacement for the 10 000-frame sweep:

      * SF9-SF12 at 125 kHz — the shipped bandwidth, and the four spreading
        factors where the mode actually engages there. This is the evidence
        round 5 needs before a profile may arm the feature.
      * SF7 at 41.7 kHz and SF8 at 62.5 kHz — the *only* way to arm the mode at
        those spreading factors at all. Engagement is a function of symbol time,
        not of SF: the sleep is a fixed two symbols and the driver refuses it
        below about 3.0 ms, so SF7 and SF8 have to be narrowed until their
        symbols are long enough. Without these two the sweep does not cover
        SF7-SF12 with the feature on, whatever its title says.
      * SF7 and SF8 at 125 kHz — the shipped channel, where the mode cannot
        engage and RadioLib silently substitutes a continuous receive. Cheap,
        and it pins the fallback: the node must report `engages=no armed=no` and
        must lose nothing.

    `shipped` is the first group alone, for a re-run that only needs the claim.
    `original` is the criterion as written in `roadmap/power/06-plan.md`, kept
    so that `--dry-run --plan original` prints what it would have cost rather
    than leaving that to be argued.
    """
    if name == "original":
        return [Point(sf, 125.0, 10000, "the criterion as written") for sf in range(7, 13)]
    shipped = [Point(sf, 125.0, frames, "shipped bandwidth, mode engages") for sf in range(9, 13)]
    if name == "shipped":
        return shipped
    if name != "claim":
        raise ValueError("unknown plan %r" % name)
    return [
        Point(7, 41.7, frames, "narrowed until the mode engages at SF7"),
        Point(8, 62.5, frames, "narrowed until the mode engages at SF8"),
    ] + shipped + [
        Point(7, 125.0, fallback_frames, "shipped channel: fallback to continuous receive"),
        Point(8, 125.0, fallback_frames, "shipped channel: fallback to continuous receive"),
    ]


def select_points(points: list, only: list) -> list:
    """Narrow a plan to the channels named on the command line.

    `sf12` takes every bandwidth at that spreading factor; `sf7@41.7` takes one.
    Matching is on the plan's own key so that a name which matches nothing is an
    empty selection the caller can refuse, rather than a silently shorter sweep.
    """
    if not only:
        return list(points)
    wanted = {o.strip().lower().replace("@", "_bw") for o in only}
    out = []
    for p in points:
        if p.key in wanted or ("sf%d" % p.sf) in wanted:
            out.append(p)
    return out


ARM_OFF = "off"
ARM_ON = "on"


def steps_for(point: Point, block: int) -> list:
    """(arm, seq) for one channel, interleaved in blocks of `block` frames.

    Both arms are run through the same minutes rather than one after the other,
    because an hour of drift on an over-the-air link is larger than the effect
    being measured: whichever arm ran while somebody walked past the antenna
    carries the difference. The arm order alternates block by block as well, so
    a monotonic drift within a block cannot favour one arm either.

    Deterministic, and that is what makes a run resumable: the step list is
    rebuilt identically on restart and the records already written index into it.
    """
    if block < 1:
        raise ValueError("a block of %d frames is not a block" % block)
    out = []
    seq = {ARM_OFF: 0, ARM_ON: 0}
    b = 0
    while seq[ARM_OFF] < point.frames or seq[ARM_ON] < point.frames:
        arms = (ARM_OFF, ARM_ON) if b % 2 == 0 else (ARM_ON, ARM_OFF)
        for arm in arms:
            for _ in range(min(block, point.frames - seq[arm])):
                out.append((arm, seq[arm]))
                seq[arm] += 1
        b += 1
    return out


# ===========================================================================
#  Loss accounting
# ===========================================================================

# What one transmit window can come to. The two exclusions are not failures and
# not successes: they are windows in which the question was not asked, and
# counting them either way would misreport the link.
RESULT_OK = "ok"                    # the counter moved by exactly one
RESULT_MISS = "miss"                # the counter did not move, after the grace re-poll
RESULT_EXTRA = "extra"              # it moved by more than one: somebody else is transmitting
RESULT_NO_TX = "no-tx"              # the sender never handed the frame to the RNode
RESULT_NODE_TX = "node-tx"          # the node transmitted during the window and was deaf


class Ledger:
    """Every window's outcome, on disk as it happens, and the sums over them.

    One JSON object per line, flushed per frame. A run that is interrupted — a
    Ctrl-C, a cable, a laptop lid — loses at most the frame in flight, and
    `--resume` rebuilds the step index from what is on disk. Nothing is held
    only in memory, because the runs this is for are measured in hours.
    """

    def __init__(self, path=None):
        self.path = path
        self._fh = None
        self.records = []

    def open(self, header: dict):
        if self.path is None:
            return
        new = not os.path.exists(self.path) or os.path.getsize(self.path) == 0
        self._fh = open(self.path, "a", encoding="utf-8")
        if new:
            self._write({"kind": "header", **header})

    def close(self):
        if self._fh:
            self._fh.close()
            self._fh = None

    def _write(self, obj: dict):
        if self._fh:
            self._fh.write(json.dumps(obj, sort_keys=True) + "\n")
            self._fh.flush()

    def load(self):
        """Replay an existing file. Returns the header, or None."""
        header = None
        if self.path is None or not os.path.exists(self.path):
            return header
        with open(self.path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except ValueError:
                    # A half-written last line is what an interrupt leaves.
                    # Dropping it is right; refusing to resume because of it
                    # would throw away the hours before it.
                    continue
                if obj.get("kind") == "header":
                    header = obj
                elif obj.get("kind") == "frame":
                    self.records.append(obj)
        return header

    def record(self, point_key: str, arm: str, seq: int, result: str, **extra):
        obj = {"kind": "frame", "point": point_key, "arm": arm, "seq": seq,
               "result": result, "t": round(time.time(), 3), **extra}
        self.records.append(obj)
        self._write(obj)

    def done(self, point_key: str, arm: str) -> set:
        """Sequence numbers already attempted for one channel and arm."""
        return {r["seq"] for r in self.records
                if r["point"] == point_key and r["arm"] == arm}

    def tally(self, point_key: str, arm: str) -> dict:
        """sent / received / missed / excluded, and which sequences were missed.

        `sent` counts only the windows in which the question was actually put:
        a frame the sender never transmitted, or one the node transmitted
        through, is excluded from both the numerator and the denominator. An
        excluded window inflating `sent` would make the miss rate look better
        than the link is; scoring it as a miss would make it look worse.
        """
        rows = [r for r in self.records if r["point"] == point_key and r["arm"] == arm]
        missed = sorted(r["seq"] for r in rows if r["result"] == RESULT_MISS)
        received = sum(1 for r in rows if r["result"] == RESULT_OK)
        extra = sorted(r["seq"] for r in rows if r["result"] == RESULT_EXTRA)
        excluded = {
            RESULT_NO_TX: sum(1 for r in rows if r["result"] == RESULT_NO_TX),
            RESULT_NODE_TX: sum(1 for r in rows if r["result"] == RESULT_NODE_TX),
        }
        return {
            "attempted": len(rows),
            "sent": received + len(missed) + len(extra),
            "received": received + len(extra),
            "missed": len(missed),
            "missed_seqs": missed,
            "extra_seqs": extra,
            "excluded": excluded,
        }


# ===========================================================================
#  Statistics
# ===========================================================================

Z_ONE_SIDED_95 = 1.6448536269514722


def wilson_upper(k: int, n: int, z: float = Z_ONE_SIDED_95) -> float:
    """Upper confidence bound on a proportion, Wilson score.

    Chosen over the textbook normal interval because every interesting result
    here has k = 0 or k = 1, where the normal interval collapses to zero width
    and would report a link proven perfect by four frames. Wilson keeps its
    nerve at the boundary: 0 misses in 500 gives 0.0065, not 0.
    """
    if n <= 0:
        return 1.0
    p = k / n
    denom = 1.0 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = (z / denom) * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return min(1.0, centre + half)


def rule_of_three_upper(n: int, conf: float = 0.95) -> float:
    """The 95 % upper bound on a rate after n trials with no failures.

    -ln(1 - conf) / n; the familiar 3/n. Quoted alongside Wilson because it is
    the figure that settles how many frames a channel needs: it is the smallest
    miss rate the run could have detected, and choosing N is choosing that.
    """
    if n <= 0:
        return 1.0
    return -math.log(1.0 - conf) / n


def excess_loss_pvalue(m_on: int, n_on: int, m_off: int, n_off: int) -> float:
    """One-sided Fisher exact p for "the duty-cycled arm lost more".

    Exact rather than approximate, because the counts are small by design and a
    chi-square on a table with one miss in it is not a statement about anything.
    The shape is the one most people would reach for — given M misses in total,
    how surprising is it that m_on of them landed in the duty-cycled arm — but
    it is hypergeometric and not binomial: the arms are finite, so it draws
    without replacement and comes out slightly stricter than the coin flip. It
    also stays correct when a run is interrupted and the arms end up uneven,
    which the coin flip does not.
    """
    m = m_on + m_off
    n = n_on + n_off
    if m == 0 or n_on == 0 or n_off == 0:
        return 1.0
    total = math.comb(n, m)
    p = 0.0
    for k in range(m_on, min(m, n_on) + 1):
        if m - k > n_off:
            continue
        p += math.comb(n_on, k) * math.comb(n_off, m - k) / total
    return min(1.0, p)


def verdict(on: dict, off: dict, armed_on: bool, armed_off: bool, engages: bool,
            max_loss: float = 0.02, alpha: float = 0.05) -> tuple:
    """Did this channel pass, and in one sentence, why.

    Three gates, and all three have to hold:

      1. the node was in the state being tested — `armed=yes` on the duty-cycled
         arm of a channel where the mode engages, `armed=no` everywhere it does
         not. A run against a receiver that quietly stayed continuous is not
         evidence about a receiver that sleeps, and this is the only way to tell
         from the outside;
      2. no statistically significant excess loss in the duty-cycled arm, by the
         exact test above;
      3. the duty-cycled arm's own loss rate is credible — the 95 % upper bound
         stays under `max_loss`. Gate 2 alone would pass a rig that lost a
         quarter of its frames in both arms equally, which proves the two arms
         are alike and nothing at all about whether the feature is safe.
    """
    if engages and not armed_on:
        return False, "the duty-cycled arm never armed: the receiver stayed continuous"
    if armed_off:
        return False, "the control arm reported the duty cycle armed"
    if not engages and armed_on:
        return False, "the mode engaged on a channel the firmware predicted it would not"
    if on["sent"] == 0 or off["sent"] == 0:
        return False, ("nothing was scored on one arm (%d on, %d off): every window was "
                       "excluded, or the channel was never run"
                       % (on["sent"], off["sent"]))
    p = excess_loss_pvalue(on["missed"], on["sent"], off["missed"], off["sent"])
    if p < alpha:
        return False, ("duty-cycled arm lost %d/%d against %d/%d (p=%.4f)"
                       % (on["missed"], on["sent"], off["missed"], off["sent"], p))
    upper = wilson_upper(on["missed"], on["sent"])
    if upper > max_loss:
        return False, ("duty-cycled loss %d/%d, 95 %% upper bound %.2f %% exceeds %.2f %%"
                       % (on["missed"], on["sent"], upper * 100, max_loss * 100))
    return True, ("%d/%d missed against %d/%d, p=%.2f, 95 %% upper bound %.2f %%"
                  % (on["missed"], on["sent"], off["missed"], off["sent"], p, upper * 100))


# ===========================================================================
#  The node, over its maintenance console
# ===========================================================================

class Node:
    """The node under test. Every command is checked; nothing is assumed applied.

    Settings on this firmware are applied live by the radio task rather than at
    the next boot, so a SET that returns OK has still only been *accepted*. Each
    one is read back through STATUS or GET before the run continues, because the
    entire experiment is a claim about which mode the receiver is in.
    """

    def __init__(self, console):
        self.console = console

    @staticmethod
    def open(target: str, timeout: float, password=None):
        from retimesh_flash import device
        if device.is_device_path(target):
            console = device.Console.open(target, timeout=timeout)
        else:
            host, port = device.split_host_port(target)
            console = device.Console.connect(host, port, timeout=timeout)
            ok, why = console.authenticate(password or "")
            if not ok:
                console.close()
                raise RuntimeError("%s: %s" % (target, why))
        return Node(console)

    def close(self):
        self.console.close()

    def command(self, line: str) -> list:
        """One command, or an exception. The node's own status word is the gate.

        Never a truthiness test on the output: a node that timed out and a node
        that answered "OK" with nothing to say both produce an empty list, and
        one of those is a dead cable.
        """
        status, kv, data = self.console.command(line)
        if status != "OK":
            raise RuntimeError("%s: %s %s" % (line, status, kv.get("text", "")))
        return data

    def version(self) -> dict:
        return self.command("VERSION")[0]

    def status(self) -> dict:
        """Every STATUS line folded into one mapping.

        The node emits several `RM STATUS ...` lines with disjoint keys, so a
        flat merge is lossless and lets a caller ask for `rx` and `armed`
        without knowing which line carried which.
        """
        out = {}
        for line in self.command("STATUS"):
            out.update(line)
        return out

    def sample(self) -> tuple:
        """(rx, tx, armed) from one STATUS.

        The receive counter, the transmit counter and whether the duty-cycled
        receive is armed *right now*, read together because they describe one
        instant. `armed` is taken per frame rather than once per block on
        purpose: it is the only field that separates a working feature from a
        silently inactive one, the chip drops out of the mode on every reception
        and every transmission, and a run that sampled it once would not notice
        a re-arm that stopped taking.

        A missing key is raised rather than defaulted to zero. A default there
        would turn a firmware that stopped reporting its receive counter into a
        run in which every single frame was missed — a result that reads as a
        damning finding and is a parsing bug.
        """
        s = self.status()
        for key in ("rx", "tx"):
            if key not in s:
                raise RuntimeError("STATUS has no %r — is this a RetiMesh node?" % key)
        return int(s["rx"]), int(s["tx"]), s.get("armed") == "yes"

    def counters(self) -> tuple:
        rx, tx, _ = self.sample()
        return rx, tx

    def get(self, key: str) -> str:
        """One setting's value.

        The node answers `RM GET radio.sf=8`, and the console's key/value reader
        takes `\\w+` for a key — which stops at the dot, so the pair arrives named
        after the last component of the key rather than the whole of it. Looked
        up that way here rather than "fixed" in the reader, which several other
        callers depend on.
        """
        leaf = key.split(".")[-1]
        for row in self.command("GET " + key):
            if leaf in row:
                return row[leaf]
        raise RuntimeError("GET %s returned nothing readable" % key)

    def set(self, key: str, value) -> None:
        self.command("SET %s %s" % (key, value))

    def set_channel(self, sf: int, bw_khz: float, freq_mhz: float, cr: int) -> dict:
        """Retune, then read back what the radio actually did.

        Bandwidth first and spreading factor second is not arbitrary: the
        validator checks the whole radio settings block on every field, and a
        channel is briefly inconsistent while one field has moved and the other
        has not. Frequency and coding rate are set before either, so the pair
        that must agree is the last thing to change.
        """
        self.set("radio.freq_mhz", "%.3f" % freq_mhz)
        self.set("radio.cr", cr)
        self.set("radio.bw_khz", "%g" % bw_khz)
        self.set("radio.sf", sf)
        return self.status()

    def set_duty_cycle(self, on: bool, settle_s: float = 2.0, tries: int = 10) -> bool:
        """Switch the receiver's mode and wait for the radio to say it took.

        `requestReconfigure()` hands the change to the radio task, which applies
        it on its next pass and only then republishes `armed=`. Returning as
        soon as SET said OK would start transmitting into a receiver that was
        still in the old mode, and the first block of every arm would be
        measuring the wrong thing.
        """
        self.set("radio.rx_duty_cycle", "on" if on else "off")
        deadline = time.monotonic() + settle_s * tries
        want = "on" if on else "off"
        while time.monotonic() < deadline:
            s = self.status()
            if s.get("rx_duty_cycle") == want:
                return s.get("armed") == "yes"
            time.sleep(settle_s / tries)
        raise RuntimeError("the node did not report rx_duty_cycle=%s after %.0fs"
                           % (want, settle_s * tries))


# ===========================================================================
#  The sender
# ===========================================================================

class Sender:
    """An RNode transmitting through Reticulum, retuned per channel.

    The sender runs in a child process, and that is not an implementation
    detail. `RNS.Reticulum.__init__` refuses a second instance in one process
    ("Attempt to reinitialise Reticulum, when it was already running"), and its
    `exit_handler` is a one-shot static that cannot be undone — so a sweep that
    changes channel six times cannot hold the stack in the harness itself. An
    RNodeInterface also takes its frequency, bandwidth and spreading factor when
    it is constructed and has no live retune, so a channel change *is* a new
    instance. One child per channel is the shape those two facts force.

    It also splits the dependencies where they already are: the parent needs
    pyserial for the node's console, the child needs RNS, and on this bench
    those live in different interpreters. `--rns-python` names the child's.

    Two modes, and the difference is who owns the radio:

      `owned`   the child writes its own Reticulum config naming the RNode and
                brings the instance up. It can therefore retune between
                channels, which is what makes an unattended sweep possible, and
                it can read the interface's own byte counter to confirm each
                frame reached the radio. It needs the RNode port to itself:
                stop the host's rnsd first.

      `shared`  the child attaches to the host's running shared instance and
                sends through whatever that has already configured. Nothing is
                retuned, so a sweep is one channel per rnsd restart, by hand —
                and a shared-instance client holds no interface objects, so
                per-frame transmit confirmation is not available and a sender
                that silently stopped would read as a receiver that stopped
                hearing. Offered because it is what a bench with a live rnsd can
                do without disturbing it, not because it is as good.
    """

    def __init__(self, mode: str, config_dir, rnode_port: str, freq_mhz: float,
                 tx_dbm: int, cr: int, python: str = None,
                 app_name: str = "retimesh", aspect: str = "hilframes"):
        self.mode = mode
        self.config_dir = Path(config_dir) if config_dir else None
        self.rnode_port = rnode_port
        self.freq_mhz = freq_mhz
        self.tx_dbm = tx_dbm
        self.cr = cr
        self.python = python or sys.executable
        self.app_name = app_name
        self.aspect = aspect
        self.proc = None
        self.confirms = False
        self._lines = None
        self._reader = None

    # -- configuration ------------------------------------------------------
    def config_text(self, sf: int, bw_khz: float) -> str:
        """The Reticulum config for one channel.

        Written to disk rather than templated in memory, so that what a run
        actually transmitted with is sitting beside its results afterwards.
        """
        return (
            "[reticulum]\n"
            "  enable_transport = No\n"
            "  share_instance = No\n"
            "  panic_on_interface_error = Yes\n"
            "[logging]\n"
            "  loglevel = 3\n"
            "[interfaces]\n"
            "  [[hil sender]]\n"
            "    type = RNodeInterface\n"
            "    interface_enabled = True\n"
            "    port = %s\n"
            "    frequency = %d\n"
            "    bandwidth = %d\n"
            "    txpower = %d\n"
            "    spreadingfactor = %d\n"
            "    codingrate = %d\n"
            % (self.rnode_port, int(round(self.freq_mhz * 1e6)), int(round(bw_khz * 1000)),
               self.tx_dbm, sf, self.cr))

    def worker_argv(self) -> list:
        return [self.python, str(Path(__file__).resolve()), "--send-worker",
                "--rns-mode", self.mode, "--app-name", self.app_name,
                "--aspect", self.aspect] + (
                ["--rns-config", str(self.config_dir)] if self.config_dir else [])

    # -- lifecycle ----------------------------------------------------------
    def tune(self, sf: int, bw_khz: float, ready_s: float = 60.0) -> str:
        """Bring the sender up on one channel, replacing whatever was there.

        Returns what the child said it opened. An RNode takes several seconds to
        accept its configuration and report ready, and starting to transmit
        before it does would spend the first block of a channel on frames the
        radio never had — so this blocks on the child's own READY line rather
        than on a sleep.
        """
        self.teardown()
        if self.mode == "owned":
            self.config_dir.mkdir(parents=True, exist_ok=True)
            (self.config_dir / "config").write_text(self.config_text(sf, bw_khz),
                                                    encoding="utf-8")
        import queue
        import subprocess
        import threading
        self._lines = queue.Queue()
        self.proc = subprocess.Popen(self.worker_argv(), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=None,
                                     text=True, bufsize=1)
        self._reader = threading.Thread(target=self._pump,
                                        args=(self.proc.stdout, self._lines),
                                        daemon=True)
        self._reader.start()
        line = self._read_line(ready_s)
        if line is None or not line.startswith("READY"):
            # It never came up, so it has nothing to shut down cleanly and
            # waiting five seconds for a goodbye it will not send only delays
            # the message the operator needs.
            self.teardown(quit_s=0.5)
            raise RuntimeError("the sender did not come up on SF%d/%g kHz: %s"
                               % (sf, bw_khz, line or "no answer"))
        self.confirms = line.split()[1:2] == ["confirmed"]
        return line

    @staticmethod
    def _pump(stdout, lines):
        """Drain the child's stdout into the queue until it closes.

        A thread rather than a timed readline() loop, because readline() on a
        pipe blocks until a line arrives or the pipe closes — a deadline around
        it is decorative, and a child that wedged with the port open would hang
        the whole sweep with no timeout ever firing.
        """
        # The queue is passed in rather than read off `self`, because teardown
        # drops the reference while this thread may still be draining a closing
        # pipe, and a background AttributeError on the bench looks like a fault
        # in the run rather than a tidy-up race.
        try:
            for line in iter(stdout.readline, ""):
                lines.put(line.strip())
        except ValueError:
            pass                             # the pipe was closed under us
        lines.put(None)

    def _read_line(self, timeout_s: float):
        """One line from the child, or None. Its words are the only gate.

        Never the process's exit status: a child that died and a child that is
        merely slow both give nothing here, and the caller's decision is the
        same for both — this frame was not transmitted, exclude the window.
        """
        import queue
        try:
            return self._lines.get(timeout=timeout_s)
        except queue.Empty:
            return None

    def teardown(self, quit_s: float = 5.0):
        """Ask the child to leave, then make sure it has.

        QUIT first, because a child that exits cleanly detaches the RNode
        interface and leaves the radio in a defined state for the next channel;
        kill after `quit_s`, because a wedged child must not hold up a sweep,
        and this is also what closes a serial port the next child needs.
        """
        proc, self.proc = self.proc, None
        self.confirms = False
        self._lines = None
        self._reader = None
        if proc is None:
            return
        try:
            if proc.poll() is None:
                proc.stdin.write("QUIT\n")
                proc.stdin.flush()
                proc.wait(timeout=quit_s)
        except Exception:
            pass
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=quit_s)
            except Exception:
                pass
        for stream in (proc.stdin, proc.stdout):
            try:
                if stream is not None:
                    stream.close()
            except Exception:
                pass

    # -- transmitting -------------------------------------------------------
    def send(self, seq: int, confirm_s: float = 20.0) -> bool:
        """Transmit one frame. True once the child says the RNode has the bytes.

        In owned mode that confirmation is Reticulum's own count of what it
        wrote to the radio, so a True means the frame reached the RNode — not
        that the RNode radiated it. It is the strongest statement available
        without a second receiver, and it is enough to separate the two failures
        that matter: a sender that stopped (the window is excluded) from a
        receiver that missed (the window is scored).
        """
        if self.proc is None:
            raise RuntimeError("no sender is running")
        self.proc.stdin.write("SEND %d\n" % seq)
        self.proc.stdin.flush()
        line = self._read_line(confirm_s)
        return parse_worker_reply(line, seq)


def parse_worker_reply(line, seq: int) -> bool:
    """Whether the child confirmed transmitting `seq`. Strict, on purpose.

    A reply for a different sequence number means the two sides have lost step,
    and treating it as a success would attribute one frame's fate to another's
    window for the rest of the channel.
    """
    if not line:
        return False
    parts = line.split()
    return len(parts) >= 2 and parts[0] == "SENT" and parts[1] == str(seq)


def send_worker(args) -> int:
    """The child process: bring Reticulum up, then transmit on command.

    Deliberately tiny and line-oriented. It writes exactly one line per request
    so the parent can gate on words rather than on timing, and it never writes
    anything else to stdout — Reticulum's own logging goes to stderr, which the
    parent inherits so an operator sees it.
    """
    if args.rns_mode == "owned" and not args.rns_config:
        print("ERR owned mode needs --rns-config", flush=True)
        return 1
    import RNS
    try:
        reticulum = (RNS.Reticulum(configdir=str(args.rns_config))
                     if args.rns_mode == "owned" else RNS.Reticulum())
    except Exception as exc:
        print("ERR %s" % exc, flush=True)
        return 1
    interface = None
    for iface in list(RNS.Transport.interfaces):
        if type(iface).__name__ == "RNodeInterface":
            interface = iface
            break
    if args.rns_mode == "owned" and interface is None:
        print("ERR Reticulum came up with no RNodeInterface", flush=True)
        return 1
    destination = RNS.Destination(None, RNS.Destination.OUT, RNS.Destination.PLAIN,
                                  args.app_name, args.aspect)
    print("READY %s" % ("confirmed" if interface is not None else "unconfirmed"),
          flush=True)

    while True:
        # readline() rather than `for line in sys.stdin`, which reads through an
        # internal buffer and would sit on a flushed request until more arrived.
        line = sys.stdin.readline()
        if not line:
            break                            # the parent closed the pipe
        line = line.strip()
        if not line or line == "QUIT":
            break
        if not line.startswith("SEND "):
            print("ERR unknown request %r" % line, flush=True)
            continue
        seq = int(line.split()[1])
        packet = RNS.Packet(destination, frame_payload(seq))
        before = getattr(interface, "txb", None)
        try:
            packet.send()
        except Exception as exc:
            print("FAIL %d %s" % (seq, exc), flush=True)
            continue
        if before is None:
            # A shared instance holds no interface object here, so there is
            # nothing to confirm against. Said once in READY, not per frame.
            print("SENT %d 0" % seq, flush=True)
            continue
        want = before + len(packet.raw)
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and getattr(interface, "txb", before) < want:
            time.sleep(0.02)
        got = getattr(interface, "txb", before) - before
        print(("SENT %d %d" if got >= len(packet.raw) else "FAIL %d %d") % (seq, got),
              flush=True)
    try:
        reticulum.exit_handler()
    except Exception:
        pass
    return 0


# ===========================================================================
#  The run
# ===========================================================================

def quiet_channel(node: Node, seconds: float, log=print) -> tuple:
    """Listen before starting: is anything else transmitting on this channel?

    A neighbour on the same spreading factor inflates the receive counter, and
    an inflated counter reads as a delivery — so a run on a busy channel cannot
    fail, whatever the feature does. This is the gate that stops that. It is
    also how a bench operator finds out that the SF8/125 kHz fallback points sit
    on the same channel as the rest of the fleet.
    """
    rx0, _, _ = node.sample()
    log("  listening %.0fs for other traffic on this channel..." % seconds)
    time.sleep(seconds)
    rx1, _, _ = node.sample()
    return rx1 == rx0, rx1 - rx0


def run_frame(node: Node, sender: Sender, seq: int, toa_ms: float,
              settle_ms: float, grace_ms: float) -> tuple:
    """One transmit window. Returns (result, facts) for the ledger.

    The grace re-poll is what keeps a slow frame from being recorded as a lost
    one. The RNode does its own carrier sense before transmitting and Reticulum
    may hold a frame behind another, so the time from `send()` to the air is not
    fixed; a single deadline would score those as misses and the run would
    report a loss rate that belonged to the sender's scheduler. Frames that
    arrive are unaffected — the second poll only happens when the first found
    nothing.
    """
    rx0, tx0, armed0 = node.sample()
    if not sender.send(seq):
        return RESULT_NO_TX, {"rx_delta": 0, "tx_delta": 0, "armed": armed0}
    time.sleep((toa_ms + settle_ms) / 1000.0)
    rx1, tx1, armed1 = node.sample()
    if rx1 == rx0 and grace_ms > 0:
        time.sleep(grace_ms / 1000.0)
        rx1, tx1, armed1 = node.sample()
    rx_delta, tx_delta = rx1 - rx0, tx1 - tx0
    # `armed` for the window is the conjunction of the readings either side of
    # it. A frame that arrived while the receiver was between modes proves
    # nothing about either, and recording the optimistic half would let a
    # channel pass on frames caught by a continuous receive.
    facts = {"rx_delta": rx_delta, "tx_delta": tx_delta, "armed": armed0 and armed1}
    if tx_delta:
        # The node transmitted inside this window, so it was deaf for part of
        # it. Whether the frame arrived or not, the window did not put the
        # question, and it is excluded either way rather than scored on the
        # side that happens to be convenient.
        return RESULT_NODE_TX, facts
    if rx_delta == 0:
        return RESULT_MISS, facts
    if rx_delta == 1:
        return RESULT_OK, facts
    return RESULT_EXTRA, facts


def hms(seconds: float) -> str:
    seconds = int(max(0, seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    return "%dh%02dm" % (h, m) if h else "%dm%02ds" % (m, s)


def describe_plan(points: list, args, out=print) -> float:
    """The table a run prints before its first frame, and its total, in seconds.

    This is the arithmetic that decides whether a criterion is runnable at all,
    so it is printed for every run and not only for a dry one: an operator
    starting a sweep should see the number of hours before the frames begin,
    not after them.

    `bound` is what the frame count buys — the 95 % upper bound on a miss rate
    the run would report as zero (the rule of three). It is the honest statement
    of what N proves, and it is the figure to argue with when choosing N: ten
    thousand frames per channel buys 0.03 %, which is an order of magnitude
    below the loss floor of any over-the-air link and is therefore precision the
    measurement cannot deliver.
    """
    total = 0.0
    out("channel plan — %d B payload, %d B on air, preamble %d symbols, CR 4/%d, %.3f MHz"
        % (PAYLOAD_BYTES, on_air_bytes(PAYLOAD_BYTES), RF_PREAMBLE_SYMS,
           args.cr, args.freq_mhz))
    out("%-4s %-8s %-8s %-8s %-9s %-8s %-9s %-7s %-8s %-8s %s"
        % ("SF", "BW kHz", "sleep us", "engages", "cycle ms", "ToA ms", "gap s",
           "frames", "bound", "per arm", "band"))
    for p in points:
        band = band_for(args.freq_mhz, p.bw_khz)
        bp = effective_basis_points(args.freq_mhz, p.bw_khz, args.duty_cycle_pct)
        gap = p.gap_s(args.freq_mhz, PAYLOAD_BYTES, args.cr, args.poll_ms,
                      args.settle_ms, args.duty_cycle_pct)
        arm = gap * p.frames
        total += 2 * arm
        out("%-4d %-8g %-8d %-8s %-9s %-8.2f %-9.2f %-7d %-8s %-8s %s @ %.2f %%"
            % (p.sf, p.bw_khz, p.sleep_us, "yes" if p.engages else "NO",
               ("%.1f" % p.cycle_ms) if p.engages else "-",
               p.toa_ms(PAYLOAD_BYTES, args.cr), gap, p.frames,
               "%.2f %%" % (100.0 * rule_of_three_upper(p.frames)), hms(arm),
               band[3] if band else "outside the plan", bp / 100.0))
    airtime = sum(2 * p.frames * p.toa_ms(PAYLOAD_BYTES, args.cr) / 1000.0 for p in points)
    engaged = sum(2 * p.frames * p.gap_s(args.freq_mhz, PAYLOAD_BYTES, args.cr,
                                         args.poll_ms, args.settle_ms,
                                         args.duty_cycle_pct)
                  for p in points if p.engages)
    out("")
    out("both arms, every channel: %s (%.1f h) — of which %s is the mode actually armed"
        % (hms(total), total / 3600.0, hms(engaged)))
    out("pure airtime inside that: %s. The rest is the sub-band's transmit allowance, "
        "which is what makes this expensive." % hms(airtime))
    if any(not p.engages for p in points):
        out("channels marked engages=NO cannot arm the mode at all: the sleep is two "
            "symbols and RadioLib refuses it under %d us, so the driver substitutes a "
            "continuous receive. They test the fallback, not the feature."
            % (RX_DC_TCXO_DELAY_US + RX_DC_TRANSITION_US))
    return total


def dry_run(points: list, args) -> int:
    describe_plan(points, args)
    print("")
    print("frames — the first three and the last of the first channel, as transmitted")
    p = points[0]
    steps = steps_for(p, args.block)
    for arm, seq in steps[:3] + [steps[-1]]:
        payload = frame_payload(seq)
        print("  %s arm, seq %5d: RNS payload %s (%d B) -> %d B on air, %.1f ms"
              % (arm, seq, payload.hex(), len(payload), on_air_bytes(len(payload)),
                 p.toa_ms(len(payload), args.cr)))
    print("")
    print("node settings this run would change and restore:")
    print("  radio.freq_mhz -> %.3f, radio.cr -> %d, radio.bw_khz and radio.sf per channel"
          % (args.freq_mhz, args.cr))
    print("  radio.rx_duty_cycle -> toggled every %d frames" % args.block)
    if not args.keep_announces:
        print("  radio.announce_interval -> 0 for the run (the node's own transmissions "
              "blind it; restored afterwards)")
    sender = Sender(args.rns_mode, args.rns_config or "<--rns-config>", args.rnode_port,
                    args.freq_mhz, args.tx_dbm, args.cr, args.rns_python,
                    args.app_name, args.aspect)
    print("")
    print("the sender runs as a child process, one per channel:")
    print("    " + " ".join(sender.worker_argv()))
    if args.rns_mode == "owned":
        print("")
        print("...against this config, rewritten per channel (shown for the first):")
        for line in sender.config_text(p.sf, p.bw_khz).splitlines():
            print("    " + line)
    print("")
    print("nothing was transmitted and no port was opened.")
    return 0


def summarise(ledger: Ledger, points: list, args, report: Reporter,
              skipped: set = frozenset()) -> None:
    print("")
    print("results")
    print("%-4s %-8s %-4s %-7s %-7s %-7s %-9s %s"
          % ("SF", "BW kHz", "arm", "sent", "recv", "missed", "excluded", "missed sequences"))
    for p in points:
        if p.key in skipped:
            continue                       # already reported, and with a better reason
        armed = {}
        for arm in (ARM_OFF, ARM_ON):
            t = ledger.tally(p.key, arm)
            scored = [r for r in ledger.records
                      if r["point"] == p.key and r["arm"] == arm
                      and r["result"] in (RESULT_OK, RESULT_MISS, RESULT_EXTRA)]
            # Deliberately asymmetric. The duty-cycled arm has to have been
            # armed for *every* frame it is credited with, because a single
            # frame caught by a continuous receive is a frame that says nothing
            # about a sleeping one. The control arm fails on *any* frame that
            # reported armed, because one is already a contradiction.
            armed[arm] = (bool(scored) and all(r.get("armed") for r in scored)
                          if arm == ARM_ON else any(r.get("armed") for r in scored))
            missed = ", ".join(str(s) for s in t["missed_seqs"][:20])
            if len(t["missed_seqs"]) > 20:
                missed += ", ... (%d more)" % (len(t["missed_seqs"]) - 20)
            print("%-4d %-8g %-4s %-7d %-7d %-7d %-9s %s"
                  % (p.sf, p.bw_khz, arm, t["sent"], t["received"], t["missed"],
                     "%d+%d" % (t["excluded"][RESULT_NO_TX], t["excluded"][RESULT_NODE_TX]),
                     missed or "-"))
        on, off = ledger.tally(p.key, ARM_ON), ledger.tally(p.key, ARM_OFF)
        ok, why = verdict(on, off, armed[ARM_ON], armed[ARM_OFF], p.engages, args.max_loss)
        report("SF%d @ %g kHz (%s)" % (p.sf, p.bw_khz,
                                       "duty-cycled" if p.engages else "fallback"), ok, why)


def execute(points: list, args, ledger: Ledger, report: Reporter) -> int:
    node = Node.open(args.node, args.timeout, args.password)
    saved = {}
    sender = Sender(args.rns_mode, args.rns_config, args.rnode_port,
                    args.freq_mhz, args.tx_dbm, args.cr, args.rns_python,
                    args.app_name, args.aspect)
    try:
        info = node.version()
        print("node: %s %s on %s via %s"
              % (info.get("firmware"), info.get("version"), info.get("board"), args.node))
        if node.status().get("supported") != "yes":
            raise RuntimeError("this node's transceiver has no duty-cycled receive "
                               "(STATUS supported=no) — there is nothing here to prove")
        for key in ("radio.freq_mhz", "radio.bw_khz", "radio.sf", "radio.cr",
                    "radio.rx_duty_cycle", "radio.announce_interval"):
            saved[key] = node.get(key)
        print("saved node settings, restored on exit: %s"
              % ", ".join("%s=%s" % kv for kv in saved.items()))
        if not args.keep_announces:
            node.set("radio.announce_interval", 0)

        skipped = set()
        for p in points:
            print("")
            print("SF%d @ %g kHz — %s" % (p.sf, p.bw_khz, p.note))
            node.set_channel(p.sf, p.bw_khz, args.freq_mhz, args.cr)
            print("  sender: %s" % sender.tune(p.sf, p.bw_khz))
            if not sender.confirms:
                print("  (this sender cannot confirm a frame reached the radio, so a "
                      "sender that stops will read as a receiver that stopped hearing)")
            quiet, heard = quiet_channel(node, args.quiet_s)
            if not quiet:
                # Not a failure of the feature, and not something to measure
                # around: a counter that moves on its own cannot tell a
                # delivery from a neighbour, so every frame after this would
                # read as received whatever the receiver did.
                skipped.add(p.key)
                report("SF%d @ %g kHz" % (p.sf, p.bw_khz), False,
                       "channel is not quiet: %d frames from elsewhere in %.0fs"
                       % (heard, args.quiet_s))
                continue
            toa = p.toa_ms(PAYLOAD_BYTES, args.cr)
            gap = p.gap_s(args.freq_mhz, PAYLOAD_BYTES, args.cr, args.poll_ms,
                          args.settle_ms, args.duty_cycle_pct)
            grace = args.grace_ms if args.grace_ms >= 0 else max(1000.0, 2 * toa)
            steps = steps_for(p, args.block)
            already = {arm: ledger.done(p.key, arm) for arm in (ARM_OFF, ARM_ON)}
            arm_now = None
            started = time.monotonic()
            for i, (arm, seq) in enumerate(steps):
                if seq in already[arm]:
                    continue
                if arm != arm_now:
                    node.set_duty_cycle(arm == ARM_ON)
                    arm_now = arm
                t0 = time.monotonic()
                result, facts = run_frame(node, sender, seq, toa, args.settle_ms, grace)
                ledger.record(p.key, arm, seq, result, **facts)
                if args.verbose or result != RESULT_OK:
                    print("    %s seq %5d: %s" % (arm, seq, result))
                elapsed = time.monotonic() - t0
                if elapsed < gap:
                    # Jitter by one full duty-cycle period on top of the pacing
                    # gap. The failure this whole run is looking for is a frame
                    # whose preamble lands inside the receiver's sleep, and that
                    # depends on the arrival *phase*. A fixed gap samples a
                    # narrow set of phases and could walk past the bad one for
                    # a thousand frames; a gap jittered by more than one period
                    # samples the phase space uniformly.
                    time.sleep(gap - elapsed
                               + random.random() * p.cycle_ms / 1000.0)
                if i % 50 == 0 and i:
                    done = sum(len(ledger.done(p.key, a)) for a in (ARM_OFF, ARM_ON))
                    rate = (time.monotonic() - started) / max(1, done)
                    print("    %d/%d frames, about %s left on this channel"
                          % (done, 2 * p.frames, hms(rate * (2 * p.frames - done))))
        summarise(ledger, points, args, report, skipped)
        return report.fails
    finally:
        sender.teardown()
        for key, value in saved.items():
            try:
                node.set(key, value)
            except Exception as exc:
                print("could not restore %s=%s: %s — set it by hand" % (key, value, exc),
                      file=sys.stderr)
        node.close()


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--node", help="the node under test: serial port or host[:port]")
    ap.add_argument("--password", default=os.environ.get("RETIMESH_PASSWORD"),
                    help="admin password, for a networked console")
    ap.add_argument("--timeout", type=float, default=4.0)
    ap.add_argument("--rnode-port", default="/dev/ttyACM1",
                    help="the sending RNode's serial port (owned mode only)")
    ap.add_argument("--rns-mode", choices=("owned", "shared"), default="owned",
                    help="own the RNode and retune it per channel, or attach to "
                         "the host's shared rnsd and run one channel as configured")
    ap.add_argument("--rns-config", help="config directory for the owned instance")
    ap.add_argument("--rns-python", default=default_rns_python(),
                    help="the interpreter the sending child runs under; it needs "
                         "RNS installed, which the parent does not")
    ap.add_argument("--app-name", default="retimesh")
    ap.add_argument("--aspect", default="hilframes")
    ap.add_argument("--send-worker", action="store_true",
                    help=argparse.SUPPRESS)
    ap.add_argument("--freq-mhz", type=float, default=RF_FREQ_MHZ)
    ap.add_argument("--cr", type=int, default=RF_CR)
    ap.add_argument("--tx-dbm", type=int, default=7)
    ap.add_argument("--duty-cycle-pct", type=int, default=0,
                    help="a manual transmit cap in percent, stricter than the "
                         "sub-band's; 0 uses the sub-band allowance the node would")
    ap.add_argument("--plan", choices=("claim", "shipped", "original"), default="claim")
    ap.add_argument("--only", action="append", default=[], metavar="SFn[@BW]",
                    help="run only these channels of the plan, e.g. --only sf12 "
                         "--only sf9@125; repeatable. Required in shared mode, "
                         "which cannot retune the sender between channels.")
    ap.add_argument("--frames", type=int, default=500,
                    help="frames per arm per channel where the mode engages")
    ap.add_argument("--fallback-frames", type=int, default=200,
                    help="frames per arm on the channels where it cannot engage")
    ap.add_argument("--block", type=int, default=25,
                    help="frames per arm between switching the duty cycle over")
    ap.add_argument("--settle-ms", type=float, default=400.0,
                    help="wait after a frame's airtime before reading the counter")
    ap.add_argument("--grace-ms", type=float, default=-1.0,
                    help="second look before scoring a miss; -1 picks 2x airtime")
    ap.add_argument("--poll-ms", type=float, default=150.0,
                    help="assumed cost of one STATUS round trip, for the estimate")
    ap.add_argument("--quiet-s", type=float, default=30.0,
                    help="listen this long for other traffic before each channel")
    ap.add_argument("--max-loss", type=float, default=0.02,
                    help="the duty-cycled arm's 95 %% upper loss bound must stay under this")
    ap.add_argument("--keep-announces", action="store_true",
                    help="leave radio.announce_interval alone (its transmissions "
                         "will blind the receiver and cost excluded windows)")
    ap.add_argument("--out", help="JSONL results file; required to resume")
    ap.add_argument("--resume", action="store_true",
                    help="continue the run in --out, skipping frames already recorded")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan, the frames and the time, touch no hardware")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.send_worker:
        return send_worker(args)

    points = select_points(build_plan(args.plan, args.frames, args.fallback_frames),
                           args.only)
    if not points:
        ap.error("--only matched no channel in the %s plan" % args.plan)

    if args.dry_run:
        return dry_run(points, args)

    if not args.node:
        ap.error("--node is required unless --dry-run")
    if args.rns_mode == "owned" and not args.rns_config:
        ap.error("--rns-config is required in owned mode: it names the directory "
                 "this run writes its Reticulum config into")
    # Checked here rather than in execute(), so a plan shared mode cannot run is
    # refused before any serial port is opened.
    if args.rns_mode == "shared" and len(points) > 1:
        ap.error("shared mode cannot retune the sending RNode, so it can only run one "
                 "channel per rnsd configuration — narrow the sweep with --only. "
                 "This plan has %d channels: %s"
                 % (len(points), ", ".join(p.key for p in points)))
    if args.resume and not args.out:
        ap.error("--resume needs --out: there is nothing to resume from")

    ledger = Ledger(args.out)
    # Read the file whether or not this is a resume. Without --resume the point
    # is to *refuse*: appending a second run's frames to the first one's file
    # would merge two different rigs into one loss rate, and the merged figure
    # would look like more evidence rather than less.
    header = ledger.load()
    if ledger.records and not args.resume:
        print("%s already holds %d frames — pass --resume to continue it, or name "
              "another file" % (args.out, len(ledger.records)), file=sys.stderr)
        return 1
    if args.resume:
        if not ledger.records:
            print("nothing to resume in %s — starting from the first frame" % args.out)
        elif header is None:
            print("warning: %s has frames but no header; resuming against the plan as "
                  "given on this command line" % args.out, file=sys.stderr)
        elif header.get("payload_bytes") != PAYLOAD_BYTES:
            # A resumed run has to be the same experiment. A payload that
            # changed size changes the airtime, the pacing and the frame the
            # receiver has to catch, and the two halves would not be comparable.
            print("%s was recorded with a %s-byte payload, this build sends %d — "
                  "these are different runs" % (args.out, header.get("payload_bytes"),
                                                PAYLOAD_BYTES), file=sys.stderr)
            return 1
    ledger.open({"started": time.time(), "plan": args.plan, "frames": args.frames,
                 "fallback_frames": args.fallback_frames, "block": args.block,
                 "freq_mhz": args.freq_mhz, "cr": args.cr, "tx_dbm": args.tx_dbm,
                 "payload_bytes": PAYLOAD_BYTES, "on_air_bytes": on_air_bytes(PAYLOAD_BYTES),
                 "node": args.node, "rns_mode": args.rns_mode})

    report = Reporter()
    describe_plan(points, args)
    try:
        return execute(points, args, ledger, report)
    except KeyboardInterrupt:
        print("")
        print("interrupted — %d frames are recorded in %s; --resume continues from there"
              % (len(ledger.records), args.out or "(nothing, no --out was given)"),
              file=sys.stderr)
        return 130
    finally:
        ledger.close()


if __name__ == "__main__":
    sys.exit(main())
