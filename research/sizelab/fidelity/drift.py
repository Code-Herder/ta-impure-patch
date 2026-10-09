#!/usr/bin/env python3
"""drift.py -- quantizing the change instead of the value: each tick's x and z change rounded
to whole world units and summed (open loop), reset to the exact value at a life's start and at
every snapshot tick (GameTime 900k + 1), against the closed loop (each value quantized against
the decoder's own last value, which is value quantization by another name).

    drift.py <datadir>
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402

d = Path(sys.argv[1])
V, offs, slot, r0, T = M.load(d)
n = len(V)
W = M.W
seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
g = r0[seg_of] + (np.arange(n) - offs[seg_of]) + 1
reset = np.zeros(n, dtype=bool)
reset[offs[:-1]] = True
reset |= (g % 900) == 1
for lab, q in (("round", lambda v: np.round(v / W) * W), ("truncate", lambda v: np.floor(v / W) * W)):
    err = np.zeros((n, 2))
    for c in range(2):
        dv = np.zeros(n)
        dv[1:] = V[1:, c] - V[:-1, c]
        qd = q(dv)
        qd[reset] = 0
        # cumulative within each reset run
        run = np.cumsum(reset) - 1
        cs = np.cumsum(qd)
        base = np.zeros(run.max() + 1)
        starts = np.nonzero(reset)[0]
        base[run[starts]] = cs[starts]
        rec = V[starts, c][np.searchsorted(starts, np.arange(n), side="right") - 1] + (cs - base[run])
        err[:, c] = (rec - V[:, c]) / W
    e = np.hypot(err[:, 0], err[:, 1])
    print(f"open-loop change quantization ({lab}): error p50 {np.percentile(e, 50):.2f} p99 "
          f"{np.percentile(e, 99):.2f} max {e.max():.1f} wu")
