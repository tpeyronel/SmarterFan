#!/usr/bin/env python3
"""Decode Novohome NH-VTR500 remote packets from an ESPHome `dump: raw` log.

Usage:  tools/decode_fan_rf.py log.txt [...]

An offline second opinion on what the firmware decoder saw. The protocol is
documented in full in PROTOCOL.md; in brief:

    335us mark, 7.7ms gap                 preamble
    then 5x { 32 data bits, ~330us stop mark, 8.79ms gap }

    tick = 252us, bit period 4 ticks
    bit 0 = 1 tick mark + 3 tick space
    bit 1 = 3 tick mark + 1 tick space

Packet, 32 bits MSB first:

    [31..12] prefix   constant 0xA1D82
    [11.. 7] KEY      which button, a lookup table (PROTOCOL.md section 4)
    [ 6.. 4] CNT      press counter, +1 mod 8 per press of any button
    [ 3.. 0] CS       (word >> 8 & 0xF) ^ (word >> 4 & 0xF) ^ 6

The press counter advances on every press of any button and is identical
across the five repeats of one press, so this script uses a change in the
counter to tell a new press from a retransmission.
"""

import re
import sys
from collections import Counter

# Nominal symbol timings in microseconds: one 252us tick, 1:3 PWM.
MARK_SHORT, MARK_LONG = 252, 756
SPACE_SHORT, SPACE_LONG = 252, 756
GAP_MIN = 5000          # anything longer than this separates frames
TOL = 0.45              # generous: we validate structurally afterwards

# This deliberately matches on width alone, with no bit-period check, where the
# firmware decoder checks both. Two different tests over the same capture make
# a disagreement between them worth investigating.

PREFIX = 0xA1D82
CHECK_XOR = 6

# Transcribed from PROTOCOL.md section 4. KEY is a lookup, not an encoding:
# the values are not contiguous and nothing about a code predicts its button.
KEYS = {
    3: "bright+", 4: "fan forward", 5: "bright-", 6: "all off",
    7: "temp+", 8: "light on/off", 9: "2H", 10: "fan 4",
    11: "temp-", 12: "fan 6", 13: "cycle full bright", 15: "fan 5",
    16: "fan 1", 17: "fan reverse", 18: "fan 2", 19: "night mode",
    21: "natural wind", 22: "fan off", 25: "4H", 28: "fan 3",
}


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
    """Split a 32-bit codeword into fields and verify the checksum."""
    word = int(bits, 2)
    n5, n6, n7 = (word >> 8) & 0xF, (word >> 4) & 0xF, word & 0xF
    key = (word >> 7) & 0x1F
    return {
        "prefix": word >> 12,
        "key": key,
        # An unlisted KEY is valid-but-unnamed: the prefix and the checksum
        # are what say a packet is real, not the button.
        "name": KEYS.get(key, "UNKNOWN"),
        "counter": (word >> 4) & 7,
        "check": n7,
        "check_ok": (n5 ^ n6 ^ n7) == CHECK_XOR,
        "prefix_ok": (word >> 12) == PREFIX,
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
              f"{'key':>3} {'button':<18} {'ctr':>3} {'chk':>3}")

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
                "" if packet["prefix_ok"] else " BAD-PREFIX",
                "" if packet["check_ok"] else " BAD-CHECK",
            ])
            print(f"{timestamp:<13} {len(frames):>6} {votes:>3}  {bits:<34} "
                  f"{packet['key']:>3} {packet['name']:<18} "
                  f"{packet['counter']:>3} {packet['check']:>3}{flags}")
            presses.append(packet)

    if not presses:
        return 1

    print(f"\n=== summary: {len(presses)} presses ===")
    print("buttons seen:", {KEYS.get(k, f"key {k}"): n for k, n in
                            Counter(p["key"] for p in presses).items()})
    bad = [p for p in presses if not (p["check_ok"] and p["prefix_ok"])]
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
