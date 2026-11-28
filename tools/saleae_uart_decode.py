#!/usr/bin/env python3
"""Decode async serial (UART) frames from a Saleae Logic 2 raw digital CSV export.

Logic 2 "Export Raw Data" writes digital.csv with one row per transition:
    Time [s],Channel 0,Channel 1,...
This tool samples one channel at the bit centres after each falling start-bit edge and
prints the bytes with their time, the gap to the previous byte and the parity/stop status.
It also lists the pulses on the other channels (--edges) so a capture can be read without
the Logic 2 GUI.

Default framing is the GW-SD3 EFR32 -> IC301 command line (PB12): 115200 baud, 8 data bits,
odd parity, 1 stop bit (SMOKE_DETECTOR/docs/re_worksheet.md section 7b).

    python tools/saleae_uart_decode.py digital.csv --channel 3
    python tools/saleae_uart_decode.py digital.csv --channel 3 --parity none --baud 9600
    python tools/saleae_uart_decode.py digital.csv --edges
"""
import argparse
import bisect
import csv
import sys

PARITIES = ("odd", "even", "none")


def load_transitions(path):
    """Returns (channel names, list of (time, levels tuple))."""
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = []
        for row in reader:
            if not row:
                continue
            rows.append((float(row[0]), tuple(int(v) for v in row[1:])))
    if not rows:
        raise ValueError(f"{path}: no data rows")
    return header[1:], rows


def channel_edges(rows, ch):
    """List of (time, level) where the channel level changes; the first entry is the start level."""
    edges = [(rows[0][0], rows[0][1][ch])]
    for t, levels in rows[1:]:
        if levels[ch] != edges[-1][1]:
            edges.append((t, levels[ch]))
    return edges


class Line:
    """Level lookup on one channel by time."""

    def __init__(self, edges):
        self.times = [t for t, _ in edges]
        self.levels = [v for _, v in edges]

    def at(self, t):
        i = bisect.bisect_right(self.times, t) - 1
        return self.levels[max(i, 0)]


def decode(edges, baud=115200.0, bits=8, parity="odd", t_min=None):
    """Decodes frames; returns a list of dicts (time, value, parity_ok, stop_ok)."""
    if parity not in PARITIES:
        raise ValueError(f"parity must be one of {PARITIES}")
    line = Line(edges)
    bit = 1.0 / baud
    frame_bits = 1 + bits + (0 if parity == "none" else 1) + 1
    out = []
    busy_until = float("-inf")
    for t, level in edges[1:]:
        if level != 0 or t < busy_until or (t_min is not None and t < t_min):
            continue
        if line.at(t + 0.5 * bit) != 0:          # glitch, not a start bit
            continue
        value = 0
        for k in range(bits):
            value |= line.at(t + (1.5 + k) * bit) << k
        pos = 1.5 + bits
        parity_ok = True
        if parity != "none":
            p = line.at(t + pos * bit)
            ones = value.bit_count() + p
            parity_ok = (ones % 2 == 1) if parity == "odd" else (ones % 2 == 0)
            pos += 1
        stop_ok = line.at(t + pos * bit) == 1
        out.append({"time": t, "value": value, "parity_ok": parity_ok, "stop_ok": stop_ok})
        busy_until = t + (frame_bits - 0.4) * bit
    return out


def pulses(edges):
    """(time, level, width_s or None) for every change after the start level."""
    out = []
    for i, (t, v) in enumerate(edges[1:], start=1):
        nxt = edges[i + 1][0] if i + 1 < len(edges) else None
        out.append((t, v, None if nxt is None else nxt - t))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("csv", help="Logic 2 raw digital export (digital.csv)")
    ap.add_argument("--channel", type=int, default=0, help="channel index to decode (default 0)")
    ap.add_argument("--baud", type=float, default=115200.0)
    ap.add_argument("--bits", type=int, default=8, choices=range(5, 10), metavar="5-9")
    ap.add_argument("--parity", default="odd", choices=PARITIES)
    ap.add_argument("--from", dest="t_min", type=float, default=None,
                    help="ignore edges before this time (s), e.g. the power-on glitch")
    ap.add_argument("--edges", action="store_true", help="list pulses on every channel instead")
    ap.add_argument("--max-edges", type=int, default=40, help="pulses listed per channel")
    args = ap.parse_args(argv)

    names, rows = load_transitions(args.csv)
    if args.edges:
        for ch, name in enumerate(names):
            ps = pulses(channel_edges(rows, ch))
            print(f"{name}: start {rows[0][1][ch]}, {len(ps)} edges")
            for t, v, w in ps[:args.max_edges]:
                width = "-" if w is None else f"{w * 1e6:.1f} us"
                print(f"  {t:12.6f} -> {v}  {width}")
        return 0

    if not 0 <= args.channel < len(names):
        print(f"channel {args.channel} not in file ({len(names)} channels)", file=sys.stderr)
        return 2
    frames = decode(channel_edges(rows, args.channel), args.baud, args.bits, args.parity, args.t_min)
    bad = 0
    last = None
    for f in frames:
        gap = "" if last is None else f"+{(f['time'] - last) * 1e3:10.3f} ms"
        flags = []
        if not f["parity_ok"]:
            flags.append("PARITY")
        if not f["stop_ok"]:
            flags.append("STOP")
        bad += bool(flags)
        print(f"{f['time']:12.6f} {gap:15} 0x{f['value']:02X} {' '.join(flags)}".rstrip())
        last = f["time"]
    print(f"{len(frames)} bytes, {bad} with framing/parity errors")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
