#!/usr/bin/env python3
"""Independent IR witness for C13 (runs on flippi).

Listens on GPIO27 (physical pin 13) through the HW-490 VS1838 module
wired + -> 3.3V (pin 1), - -> GND (pin 9), S -> pin 13, with the Pi's
internal pull-up enabled (required: without it the line rises too
slowly and the spaces between pulses smear away). Captures edges with
kernel timestamps via gpiod for a fixed window, decodes NEC frames,
and writes a JSON verdict: {edges, frames: [{kind, addr, cmd}]}.

This is the physical judge for the bridge's finite TX: software
counters on the Flipper say what the provider did; only this receiver
says what actually left the LED.

Usage: witness_ir.py --seconds 30 --out /tmp/witness.json
"""
import argparse
import json
import time

import gpiod
from gpiod.line import Bias, Direction, Edge

GPIO = 27
CHIP = "/dev/gpiochip0"


def nec_decode(pulses):
    """pulses: [(level, dur_us)] for one burst. Returns a frame dict
    or None. NEC: 9ms mark + 4.5ms space leader, 560us marks, space
    560us=0 / 1690us=1, LSB first, addr/~addr/cmd/~cmd."""
    seq = [d for (_lvl, d) in pulses]
    if len(seq) < 3:
        return None

    def near(x, t, tol=0.35):
        return abs(x - t) <= t * tol

    if not near(seq[0], 9000):
        return None
    if near(seq[1], 2250):
        return {"kind": "repeat"}
    if not near(seq[1], 4500):
        return None
    bits = []
    i = 2
    while i + 1 < len(seq) and len(bits) < 32:
        mark, space = seq[i], seq[i + 1]
        if not near(mark, 560, 0.45):
            break
        bits.append(1 if space > 1125 else 0)
        i += 2
    if len(bits) < 32:
        return None
    by = []
    for k in range(4):
        v = 0
        for b in range(8):
            v |= bits[k * 8 + b] << b
        by.append(v)
    if (by[0] ^ by[1]) != 0xFF or (by[2] ^ by[3]) != 0xFF:
        return None
    return {"kind": "data", "addr": by[0], "cmd": by[2]}


def decode_all(edges):
    pulses = []
    for i in range(1, len(edges)):
        dur_us = (edges[i][0] - edges[i - 1][0]) / 1000.0
        pulses.append((edges[i - 1][1], dur_us))
    frames = []
    cur = []
    for lvl, d in pulses:
        cur.append((lvl, d))
        if lvl == 1 and d > 20000:  # long space: burst boundary
            f = nec_decode(cur)
            if f is not None:
                frames.append(f)
            cur = []
    if cur:
        f = nec_decode(cur)
        if f is not None:
            frames.append(f)
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--out", default="/tmp/witness.json")
    args = ap.parse_args()

    req = gpiod.request_lines(
        CHIP,
        consumer="irwitness",
        config={
            GPIO: gpiod.LineSettings(
                direction=Direction.INPUT,
                edge_detection=Edge.BOTH,
                bias=Bias.PULL_UP,
            )
        },
    )
    edges = []
    start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    end = start + int(args.seconds * 1e9)
    try:
        while True:
            remain = (end - time.clock_gettime_ns(time.CLOCK_MONOTONIC)) / 1e9
            if remain <= 0:
                break
            if req.wait_edge_events(remain):
                for e in req.read_edge_events(256):
                    if e.timestamp_ns >= start:
                        edges.append(
                            (e.timestamp_ns,
                             1 if e.event_type == e.Type.RISING_EDGE else 0)
                        )
    finally:
        req.release()

    frames = decode_all(edges)
    verdict = {"edges": len(edges), "frames": frames}
    with open(args.out, "w") as fh:
        json.dump(verdict, fh)
    data = [f for f in frames if f["kind"] == "data"]
    reps = [f for f in frames if f["kind"] == "repeat"]
    print(f"witness: edges={len(edges)} data={len(data)} repeats={len(reps)}",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
