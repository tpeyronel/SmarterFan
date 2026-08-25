#!/usr/bin/env python3
"""Convert an adc_logger dump in an ESPHome log into a CSV of volts.

    tools/adclog_to_csv.py log8.txt -o cold.csv
    tools/scope_decode.py cold.csv

The firmware prints a header line and then the ring buffer as 12-bit hex:

    [adclog]: BEGIN n=32768 rate=40000 atten=12dB divider=2.0
    [adclog]: D 7FF7FE800801...
    [adclog]: END

Counts are converted back to volts at the pad, undoing both the ADC's
attenuation and the external divider, so the numbers are directly comparable
with a scope probe on the pad.
"""

import re
import sys

# ESP32-C3, 12 dB attenuation: full scale is nominally 3.1 V at 12 bits.
# This is the datasheet figure, not a calibration -- treat absolute voltages as
# +/-5% until checked against a meter. Ratios and timing are unaffected.
ADC_FULL_SCALE_V = 3.1
ADC_MAX_COUNT = 4095


def parse(path, which="largest"):
    """A log may hold many dumps; keep the one with the most samples.

    on_raw fires several times per press, so short dumps are common and the
    useful one is whichever accumulated the most history.
    """
    dumps, header, digits = [], None, []

    def flush():
        if header is None or not digits:
            return
        # Lines carry an index. A gap means the logger dropped output, and
        # concatenating across it would splice non-adjacent samples together --
        # silently corrupting the timing everything downstream depends on.
        indexed = [d for d in digits if d[0] is not None]
        gaps = 0
        if indexed:
            expected = indexed[0][0]
            kept = []
            for idx, blob in indexed:
                if idx == expected:
                    kept.append(blob)
                    expected += 1
                else:
                    gaps += idx - expected
                    break          # stop at the first gap: timing is intact up to here
            body = kept
        else:
            body = [b for _, b in digits]
        blob = "".join(body)
        counts = [int(blob[i:i + 3], 16) for i in range(0, len(blob) - 2, 3)]
        if counts:
            dumps.append((header, counts, gaps))

    for line in open(path, errors="replace"):
        m = re.search(r"BEGIN n=(\d+) rate=(\d+).*?divider=([\d.]+)", line)
        if m:
            flush()
            header = (int(m.group(1)), int(m.group(2)), float(m.group(3)))
            digits = []
            continue
        m = re.search(r"\]: D (?:(\d+) )?([0-9A-Fa-f]+)\s*$", line)
        if m and header:
            digits.append((int(m.group(1)) if m.group(1) else None, m.group(2)))
    flush()

    if not dumps:
        raise SystemExit(f"{path}: no 'BEGIN n=... rate=...' line found. "
                         "Is the adc_logger dump in this log?")
    sizes = sorted(len(d[1]) for d in dumps)
    print(f"  {len(dumps)} dump(s) in this log; sample counts "
          f"{sizes[0]}..{sizes[-1]}")
    best = max(dumps, key=lambda d: len(d[1]))
    if best[2]:
        print(f"  NOTE: truncated at the first dropped line ({best[2]} lines "
              f"missing after it); kept the contiguous run so timing stays exact")
    return best[0], best[1]


def main(argv):
    path = argv[0]
    out = "adc.csv"
    if "-o" in argv:
        out = argv[argv.index("-o") + 1]
    (n_expected, rate, divider), counts = parse(path)

    print(f"  declared {n_expected} samples at {rate} Hz, divider {divider}x")
    print(f"  recovered {len(counts)} samples ({len(counts)/rate*1000:.1f} ms)")
    if len(counts) < n_expected:
        print(f"  WARNING: {n_expected - len(counts)} samples missing — the log "
              "was probably truncated. Capture with `esphome logs` redirected "
              "to a file rather than scrolling a terminal.")

    lo, hi = min(counts), max(counts)
    to_v = lambda c: c / ADC_MAX_COUNT * ADC_FULL_SCALE_V * divider
    print(f"  counts {lo}-{hi}  ->  pad {to_v(lo):.3f} V to {to_v(hi):.3f} V")
    if hi >= ADC_MAX_COUNT - 8:
        print("  WARNING: clipping at full scale. Increase the divider ratio.")
    if hi - lo < 200:
        print("  WARNING: very little signal swing — check the divider wiring.")

    dt = 1.0 / rate
    with open(out, "w") as fh:
        fh.write("Time(s),Pad(V)\n")
        for i, c in enumerate(counts):
            fh.write("%.9f,%.4f\n" % (i * dt, to_v(c)))
    print(f"\n  wrote {out}\n  now run:  python3 tools/scope_decode.py {out}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__.strip())
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
