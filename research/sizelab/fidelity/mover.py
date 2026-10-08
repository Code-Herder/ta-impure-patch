#!/usr/bin/env python3
"""mover.py -- the mover's motion fields, exact, under three predictions: what a lossy unit
stream could drop from them, against what a lossless one keeps for a few bytes.

    mover.py <datadir>

  A  each field against its own previous value (rest.py's coding)
  B  velocity x/z against the position's step at the same tick (the placement adds the velocity
     to the position, 0x43D7FF..0x43D839), the turn rate w24 against the heading's step, the
     speed against its previous value
  C  speed and w24 alone (what a lossy stream that derives the velocity from its positions keeps)
Framing as rest.py: per tick the changed units' slot gaps and a mask of the changed fields, then
the residual columns, compressed per 300-tick block.
"""
import sys
from pathlib import Path

import numpy as np
import zstandard
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from motion import varint, BLOCK  # noqa: E402

d = Path(sys.argv[1])
L = np.load(d / "live.npy").astype(bool)
T, S = L.shape
both = np.zeros_like(L)
both[1:] = L[1:] & L[:-1]


def F(n):
    return np.load(d / f"f_{n}.npy").astype(np.int64)


x, z, hd, vx, vy, vz, spd, w24 = (F(n) for n in ("x", "z", "hd", "vx", "vy", "vz", "spd", "w24"))
dx = np.zeros_like(x); dx[1:] = x[1:] - x[:-1]
dz = np.zeros_like(z); dz[1:] = z[1:] - z[:-1]
dh = np.zeros_like(hd); dh[1:] = ((hd[1:] - hd[:-1] + 32768) % 65536) - 32768
prev = lambda a: np.concatenate((a[:1], a[:-1]))  # noqa: E731


def size(fields):
    """fields: list of (value [T,S], prediction [T,S]); a unit is written at t when any value
    differs from its previous one or from its prediction."""
    zc = zstandard.ZstdCompressor(level=19)
    raw = zs = df = n = 0
    tail = 0
    for b0 in range(0, T, BLOCK):
        ts, cnts, gaps, masks = [], [], [], []
        cols = [[] for _ in fields]
        for t in range(max(1, b0), min(T, b0 + BLOCK)):
            lb = both[t]
            res = [v[t] - p[t] for v, p in fields]
            chg = np.zeros(S, dtype=bool)
            for (v, p), r in zip(fields, res):
                chg |= ((v[t] != v[t - 1]) | (r != 0)) & lb
            sl = np.nonzero(chg)[0]
            if not len(sl):
                continue
            n += len(sl)
            ts.append(t - b0)
            cnts.append(len(sl))
            gaps.append(np.diff(np.concatenate(([0], sl))))
            m = np.zeros(len(sl), dtype=np.int64)
            for i, r in enumerate(res):
                nz = r[sl] != 0
                m |= nz.astype(np.int64) << i
                cols[i].append(r[sl][nz])
            masks.append(m)
        if not ts:
            continue
        data = varint(np.array(ts)) + varint(np.array(cnts)) + varint(np.concatenate(gaps)) + \
            varint(np.concatenate(masks)) + b"".join(varint(np.concatenate(c)) for c in cols)
        raw += len(data)
        zb = len(zc.compress(data))
        zs += zb
        if 4500 <= b0 < 7200:
            tail += zb
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        df += len(c.compress(data) + c.flush())
    return n, raw, zs, df, tail


def show(label, r):
    n, raw, zs, df, tail = r
    print(f"{label:<58} units written {n:>9,}  raw {raw / 1e6:6.2f}  zstd19 {zs / 1e6:5.2f} MB"
          f"  deflate2k {df / 1e6:5.2f}  tail {tail / 3 / 1e6 * 120:5.1f} MB/h", flush=True)


show("A: vx vy vz speed w24, each against its previous",
     size([(vx, prev(vx)), (vy, prev(vy)), (vz, prev(vz)), (spd, prev(spd)), (w24, prev(w24))]))
vxp = np.where(dx != 0, dx, prev(vx))
vzp = np.where(dz != 0, dz, prev(vz))
show("B: v against the position step, w24 against the heading step",
     size([(vx, vxp), (vy, prev(vy)), (vz, vzp), (spd, prev(spd)), (w24, dh)]))
show("C: speed and w24 only (velocity derived from positions)",
     size([(spd, prev(spd)), (w24, dh)]))
show("C': speed only", size([(spd, prev(spd))]))
