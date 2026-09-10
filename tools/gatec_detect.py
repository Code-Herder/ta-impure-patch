#!/usr/bin/env python3
"""Gate C's transient detector (research/notes/gpu-posing.md §4 step 7).

A frame is ranked by the pixels where it **differs from both neighbours while the
neighbours agree with each other**.  That test rejects smooth motion and animated
water, which is the whole reason it is used rather than a plain frame delta.

It runs over the WORLD BAND only — the viewport less its top 120 px.  The frame's top
carries the engine's message log, which scrolls on its own and produced the largest
transient of one G13 capture with no unit ever drawn wrong.

**A RANKED FRAME IS A CANDIDATE, NOT A FINDING.**  A rotating unit passes this test
too: k-1 and k+1 can resemble each other while k sits between them, and at the 30 Hz
sim rate presented at 60 Hz every leg swing is an odd frame out.  Every ranked frame on
a walking commander up to ~600 px has turned out to be ordinary gait.  So the script
also reports the densest 128x128 window: a wrong POSE is one dense cluster on a unit,
foliage and water shimmer is scattered.  Open the strip before believing the number —
the rest-pose draw's signature is the body's ORIENTATION lost for one frame, and no
pixel count can tell you that.

The bar: frames over 1000 changed pixels -> 0 (the original artifact measured 1403 px).
The >350 band is the walk itself and does not move.  Whether the >500 band discriminates
depends on the fixture: on `scenarios/walk-gatec.json` the walk's own ceiling is ~520 px,
so it does not.
"""
import subprocess
import sys

import numpy as np

W, H = 1920, 1080
# The viewport is (128, 32, 1792x1016) at this resolution; the message log lives in the
# top of it, so the band starts 120 px lower.
X0, X1, Y0, Y1 = 128, 1920, 152, 1048
BOX = 128


def frames(path):
    p = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", path,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE, bufsize=10 ** 8)
    n = W * H * 3
    try:
        while True:
            b = p.stdout.read(n)
            if len(b) < n:
                return
            yield np.frombuffer(b, np.uint8).reshape(H, W, 3)[Y0:Y1, X0:X1]
    finally:
        p.stdout.close()
        p.wait()


def densest(mask):
    """The BOX x BOX window holding the most of `mask`, via a summed-area table."""
    i = np.pad(np.cumsum(np.cumsum(mask.astype(np.int32), 0), 1), ((1, 0), (1, 0)))
    s = (i[BOX:, BOX:] - i[:-BOX, BOX:] - i[BOX:, :-BOX] + i[:-BOX, :-BOX])
    k = int(s.argmax())
    y, x = divmod(k, s.shape[1])
    return int(s.flat[k]), (x + X0 + BOX // 2, y + Y0 + BOX // 2)


def main(path):
    it = frames(path)
    a = next(it, None)
    b = next(it, None)
    if b is None:
        sys.exit(f"{path}: fewer than three frames")
    rows = []
    for k, c in enumerate(it, start=1):
        m = (a != b).any(2) & (b != c).any(2) & (a == c).all(2)
        t = int(m.sum())
        rows.append((k, t) + (densest(m) if t > 300 else (None, None)))
        a, b = b, c

    over = lambda th: sum(1 for r in rows if r[1] > th)
    print(f"  {path.split('/')[-1]}: {len(rows) + 2} frames, band {X1 - X0}x{Y1 - Y0}")
    print(f"    >350: {over(350)}   >500: {over(500)}   >1000: {over(1000)}")
    for k, t, d, loc in sorted(rows, key=lambda r: -r[1])[:12]:
        if t > 300:
            print(f"    f{k:<5d} {t:5d} px   densest {BOX}x{BOX}: {d:5d} "
                  f"({100 * d // t:3d}%) at {loc}")
    if over(1000):
        print("    ^ OVER THE BAR. Open the strip on those frames before reporting a "
              "regression: only a lost body orientation is the pose artifact.")


if __name__ == "__main__":
    main(sys.argv[1])
