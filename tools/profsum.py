#!/usr/bin/env python3
"""Summarise a frame profile: tagpu_prof_game.csv or tagpu_prof_render.csv (tagpu_prof.h).

    tools/profsum.py <file.csv> [...]          one table a file
    tools/profsum.py --min-share 0 <file.csv>  every span, the empty ones too

A row is one frame: its start in ms since the lane was armed, its total in microseconds, the
ticks it ran (`count`), a tag (the game lane's GameTime), then microseconds per span. The table
gives, per span, the mean a frame, its share of the window, the median and the 99th percentile,
and for the game lane the mean a tick. The first row is dropped: it starts at whatever the lane
was doing when the lever was seen."""
import argparse, csv, sys


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    i = min(len(sorted_vals) - 1, int(p * len(sorted_vals)))
    return sorted_vals[i]


def summarise(path, min_share):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if len(rows) < 3:
        print(f"{path}: no frames")
        return
    head, rows = rows[0], rows[2:]
    # a last row cut by a copy taken mid-write has fewer cells than the header
    rows = [r for r in rows if len(r) == len(head)]
    spans = head[5:]
    total = [int(r[2]) for r in rows]
    ticks = sum(int(r[3]) for r in rows)
    wall_ms = sum(total) / 1000.0
    n = len(rows)
    print(f"{path}")
    print(f"  {n} frames over {wall_ms / 1000.0:.1f} s: {n / (wall_ms / 1000.0):.1f} frames a second, "
          f"{sum(total) / n / 1000.0:.2f} ms a frame (median {pct(sorted(total), 0.5) / 1000.0:.2f}, "
          f"p99 {pct(sorted(total), 0.99) / 1000.0:.2f})"
          + (f"; {ticks} ticks, {ticks / (wall_ms / 1000.0):.1f} a second" if ticks else ""))
    print(f"  {'span':<14}{'ms/frame':>10}{'share':>8}{'median':>10}{'p99':>10}"
          + (f"{'ms/tick':>10}" if ticks else ""))
    table = []
    for k, name in enumerate(spans):
        col = [int(r[5 + k]) for r in rows]
        s = sum(col)
        table.append((s, name, sorted(col)))
    for s, name, col in sorted(table, reverse=True):
        share = s / sum(total) if sum(total) else 0.0
        if share * 100.0 < min_share:
            continue
        line = (f"  {name:<14}{s / n / 1000.0:>10.3f}{share * 100.0:>7.1f}%"
                f"{pct(col, 0.5) / 1000.0:>10.3f}{pct(col, 0.99) / 1000.0:>10.3f}")
        if ticks:
            line += f"{s / ticks / 1000.0:>10.3f}"
        print(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--min-share", type=float, default=0.1,
                    help="leave out spans under this percent of the window (default 0.1)")
    a = ap.parse_args()
    for p in a.files:
        summarise(p, a.min_share)
    return 0


if __name__ == "__main__":
    sys.exit(main())
