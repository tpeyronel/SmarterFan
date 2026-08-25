#!/usr/bin/env python3
"""Decode Novohome NH-VTR500 remote packets from an ESPHome `dump: raw` log.

Usage:  tools/decode_fan_rf.py log2.txt [...]

Wire format (reverse-engineered from log.txt + log2.txt, 21:58 and 23:05
capture sessions):

    335us mark, 7.7ms gap                 preamble
    then 5x { 32 data bits, ~330us stop mark, 8.79ms gap }

    bit 0 = 269us mark + 740us space      bit period ~1009us
    bit 1 = 768us mark + 242us space

Packet (32 bits, MSB first):

    [ 0..15] 0xA1D8   remote / device ID
    [16..23] command  0x21 brightness+, 0x23 colour temp+, 0x28 fan speed 1
    [24..27] high nibble = [domain flag][3-bit press counter]
    [28..31] low nibble  = high nibble XOR (command & 0x0F) XOR 6

The press counter advances +1 mod 8 on every press of any button, and is
identical across the five repeats of one press.

The domain flag is 1 for the light commands (0x21, 0x23) and 0 for the fan
command (0x28) — i.e. the inverse of command bit 3, consistent with the
command space being split 0x20-0x27 light / 0x28-0x2F fan. Bit 28 reads as a
constant 1 in every packet so far, but that is a *consequence* of the nibble
rule rather than a marker: flag XOR (command bit 3) is 1 by construction.
"""

import re
import sys
from collections import Counter

# Nominal symbol timings in microseconds, measured over uncorrupted frames.
MARK_SHORT, MARK_LONG = 269, 768
SPACE_SHORT, SPACE_LONG = 242, 740
GAP_MIN = 5000          # anything longer than this separates frames
TOL = 0.45              # generous: we validate structurally afterwards

DEVICE_ID = 0xA1D8
COMMANDS = {0x21: "brightness+", 0x23: "colour temp+", 0x28: "fan speed 1"}


def parse_captures(text):
    """Yield (timestamp, [signed durations]) per 'Received Raw:' block.

    Continuation lines carry the same log tag but no 'Received Raw:' prefix.
    """
    captures, current = [], None
    for line in text.splitlines():
        m = re.search(r"remote\.raw:\d+\]: (.*)$", line)
        if not m:
            continue
        body = m.group(1)
        if body.startswith("Received Raw:"):
            if current:
                captures.append(current)
            current = (line[1:13], [])
            body = body[len("Received Raw:"):]
        if current is None:
            continue
        current[1].extend(int(x) for x in re.findall(r"-?\d+", body))
    if current:
        captures.append(current)
    return captures


def split_frames(durations):
    """Split a capture on inter-frame gaps. First chunk is the preamble."""
    frames, chunk = [], []
    for value in durations:
        if value < -GAP_MIN:
            frames.append(chunk)
            chunk = []
        else:
            chunk.append(value)
    frames.append(chunk)
    return frames


def _near(value, nominal):
    return abs(value - nominal) <= TOL * nominal


def decode_frame(frame):
    """Return a 32-char bitstring, or None if the frame is not clean.

    A frame is 32 (mark, space) pairs followed by a lone stop mark.
    """
    if len(frame) < 64:
        return None
    bits = []
    for i in range(0, 64, 2):
        mark, space = frame[i], -frame[i + 1]
        if _near(mark, MARK_LONG) and _near(space, SPACE_SHORT):
            bits.append("1")
        elif _near(mark, MARK_SHORT) and _near(space, SPACE_LONG):
            bits.append("0")
        else:
            return None
    return "".join(bits)


def decode_packet(bits):
    """Split a 32-bit codeword into fields and verify the check nibble."""
    device = int(bits[0:16], 2)
    command = int(bits[16:24], 2)
    high = int(bits[24:28], 2)          # [domain flag][counter:3]
    low = int(bits[28:32], 2)           # check nibble
    return {
        "device": device,
        "command": command,
        "name": COMMANDS.get(command, "UNKNOWN"),
        "domain": high >> 3,
        "counter": high & 7,
        "check": low,
        "check_ok": low == (high ^ (command & 0x0F) ^ 6),
        "domain_ok": (high >> 3) == (0 if command & 0x08 else 1),
        "device_ok": device == DEVICE_ID,
    }


def main(paths):
    presses = []
    for path in paths:
        with open(path) as handle:
            captures = parse_captures(handle.read())
        if not captures:
            print(f"{path}: no 'Received Raw:' lines found", file=sys.stderr)
            continue

        print(f"\n=== {path}: {len(captures)} captures ===")
        print(f"{'time':<13} {'frames':>6} {'ok':>3}  {'code':<34} "
              f"{'cmd':<14} {'ctr':>3} {'chk':>3}")

        for timestamp, durations in captures:
            frames = split_frames(durations)[1:]      # drop preamble
            decoded = [b for b in map(decode_frame, frames) if b]
            if not decoded:
                print(f"{timestamp:<13} {len(frames):>6} {0:>3}  "
                      f"{'-- no clean frame --':<34}")
                continue

            # The repeats are identical; majority-vote to shrug off glitches.
            bits, votes = Counter(decoded).most_common(1)[0]
            packet = decode_packet(bits)
            flags = "".join([
                "" if packet["device_ok"] else " BAD-ID",
                "" if packet["domain_ok"] else " BAD-DOMAIN",
                "" if packet["check_ok"] else " BAD-CHECK",
            ])
            print(f"{timestamp:<13} {len(frames):>6} {votes:>3}  {bits:<34} "
                  f"0x{packet['command']:02X} {packet['name']:<9} "
                  f"{packet['counter']:>3} {packet['check']:>3}{flags}")
            presses.append(packet)

    if not presses:
        return 1

    print(f"\n=== summary: {len(presses)} presses ===")
    print("commands seen:", {f"0x{c:02X}": n for c, n in
                             Counter(p["command"] for p in presses).items()})
    bad = [p for p in presses if not (p["check_ok"] and p["device_ok"]
                                      and p["domain_ok"])]
    print(f"field checks: {len(presses) - len(bad)}/{len(presses)} pass")

    counters = [p["counter"] for p in presses]
    steps = Counter((b - a) % 8 for a, b in zip(counters, counters[1:]))
    print("counter increments between consecutive presses:", dict(steps))
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(args))
