#!/usr/bin/env python3
"""Plot a continuous adc_logger stream from an ESPHome log.

    .venv/bin/python tools/plot_adc.py log13.txt
    .venv/bin/python tools/plot_adc.py log13.txt --from 1.2 --to 1.6
    .venv/bin/python tools/plot_adc.py log13.txt -o plot.png

The firmware emits one line per 64 samples, each tagged with the absolute
index of its first sample:

    [adclog]: STREAM rate=10000 channel=4 atten=12dB divider=2.0
    [adclog]: S 0 7FF7FE800...
    [adclog]: S 64 801800...

Indices matter. If the logger falls behind, whole lines are dropped, and
plotting the survivors end to end would silently compress the time axis and
make the trace lie. Missing runs are left as gaps instead.
"""

import argparse
import re
import sys

import numpy as np

ADC_FULL_SCALE_V = 3.1     # ESP32-C3, 12 dB attenuation (datasheet, ~5% accurate)
ADC_MAX_COUNT = 4095
SAMPLES_PER_LINE = 64


def parse(path, rate_override=None, divider_override=None):
    rate, divider, chunks = None, 2.0, {}
    for line in open(path, errors="replace"):
        m = re.search(r"STREAM rate=(\d+).*?divider=([\d.]+)", line)
        if m:
            rate, divider = int(m.group(1)), float(m.group(2))
            continue
        m = re.search(r"\]: S (\d+) ([0-9A-Fa-f]+)\s*$", line)
        if m:
            start = int(m.group(1))
            blob = m.group(2)
            vals = [int(blob[i:i + 3], 16) for i in range(0, len(blob) - 2, 3)]
            if vals:
                chunks[start] = vals
    if not chunks:
        raise SystemExit(f"{path}: no 'S <index> <hex>' sample lines found. "
                         "Is this an adc_logger stream log?")
    if rate_override:
        rate = rate_override
    if divider_override:
        divider = divider_override
    if rate is None:
        # The header is only emitted every few seconds, so a short capture --
        # or one attached after boot -- can legitimately miss it.
        rate = 10000
        print(f"  WARNING: no 'STREAM rate=' header in this log; assuming "
              f"{rate} Hz and divider {divider}x. Pass --rate/--divider if the "
              "firmware is configured differently, or the time axis will be wrong.")
    return rate, divider, chunks


def build(chunks, rate):
    """Lay chunks on a timeline, leaving NaN where lines were lost.

    Indices are absolute since the firmware booted, so a log attached later
    starts at a large offset. Rebase to the first sample actually received --
    otherwise the plot opens with minutes of empty axis.
    """
    origin = min(chunks)
    last = max(chunks)
    total = last - origin + len(chunks[last])
    data = np.full(total, np.nan)
    for start, vals in chunks.items():
        data[start - origin:start - origin + len(vals)] = vals
    return data, origin


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("-o", "--out", help="write a PNG instead of opening a window")
    ap.add_argument("--from", dest="t0", type=float, default=None, help="seconds")
    ap.add_argument("--to", dest="t1", type=float, default=None, help="seconds")
    ap.add_argument("--counts", action="store_true", help="plot raw ADC counts")
    ap.add_argument("--rate", type=int, help="sample rate if the log has no header")
    ap.add_argument("--divider", type=float, help="external divider ratio")
    args = ap.parse_args()

    rate, divider, chunks = parse(args.logfile, args.rate, args.divider)
    data, origin = build(chunks, rate)
    missing = int(np.isnan(data).sum())
    dur = len(data) / rate

    print(f"  rate {rate} Hz, divider {divider}x")
    print(f"  {len(data)} sample slots, {dur:.3f} s")
    if origin:
        print(f"  stream began {origin/rate:.1f} s before this log starts "
              f"(first index {origin}); plot time is relative to the log")
    if missing:
        print(f"  {missing} samples missing ({100*missing/len(data):.2f}%) — "
              "shown as gaps, not closed up")
    else:
        print("  no gaps: every line arrived")

    volts = data / ADC_MAX_COUNT * ADC_FULL_SCALE_V * divider
    series = data if args.counts else volts
    label = "ADC counts" if args.counts else "volts at the pad"
    finite = series[np.isfinite(series)]
    if finite.size:
        print(f"  range {finite.min():.3f} .. {finite.max():.3f} {label}")

    t = np.arange(len(series)) / rate
    lo = 0 if args.t0 is None else max(0, int(args.t0 * rate))
    hi = len(series) if args.t1 is None else min(len(series), int(args.t1 * rate))
    t, series = t[lo:hi], series[lo:hi]

    import matplotlib
    if args.out:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(16, 5))
    ax.plot(t, series, linewidth=0.4)
    ax.set_xlabel("seconds")
    ax.set_ylabel(label)
    ax.set_title(f"{args.logfile} — GPIO ADC stream, {rate} Hz"
                 + (f", {missing} samples missing" if missing else ""))
    ax.grid(alpha=0.3)
    if not args.counts:
        # The digital tap's decision level, for comparison with what the RMT saw.
        ax.axhline(1.65 * divider, color="red", linestyle="--", linewidth=0.8,
                   label="ESP32 input threshold referred to the pad")
        ax.legend(loc="upper right", fontsize=8)
    fig.tight_layout()

    if args.out:
        fig.savefig(args.out, dpi=130)
        print(f"\n  wrote {args.out}")
    else:
        print("\n  opening window — zoom with the toolbar; close it to exit")
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
