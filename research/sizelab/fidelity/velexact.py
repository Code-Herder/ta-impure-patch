#!/usr/bin/env python3
"""velexact.py -- the velocity a decoder derives from 4-bit-truncated positions (the step from the
tick before), against the recorded one; and what an exact point every second costs to carry the
true velocity as the difference from the derived one (zigzag varints, zstd 19 per 300 ticks).

    velexact.py <datadir>
"""
import sys
from pathlib import Path

import numpy as np
import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
from motion import varint  # noqa: E402

d = Path(sys.argv[1])
L = np.load(d / "live.npy").astype(bool)
T, S = L.shape
both = np.zeros_like(L)
both[1:] = L[1:] & L[:-1]
F = lambda n: np.load(d / f"f_{n}.npy").astype(np.int64)  # noqa: E731
x, z, a, vx, vy, vz = (F(n) for n in ("x", "z", "alt", "vx", "vy", "vz"))
q = 1 << 12
der = []
for p in (x, a, z):
    pq = (p // q) * q
    dv = np.zeros_like(p)
    dv[1:] = pq[1:] - pq[:-1]
    der.append(dv)
vel = (vx, vy, vz)
m = both & ((vx != 0) | (vz != 0) | (vy != 0) | (der[0] != 0) | (der[2] != 0))
eq = m & (vx == der[0]) & (vz == der[2]) & (vy == der[1])
print(f"unit-ticks with a velocity or a step: {m.sum():,}; derived == recorded {100 * eq.sum() / m.sum():.1f} %")
err = np.hypot((vx - der[0]) / 65536, (vz - der[2]) / 65536)[m]
print(f"  derived velocity error, wu/tick: p50 {np.percentile(err, 50):.3f} p99 {np.percentile(err, 99):.3f} max {err.max():.2f}")
gt = np.arange(1, T + 1)
zc = zstandard.ZstdCompressor(level=19)
tot = 0
for b0 in range(0, T, 300):
    parts = []
    for t in range(b0, min(T, b0 + 300)):
        if gt[t] % 30 != 1:
            continue
        sl = np.nonzero(m[t])[0]
        parts.append(varint(np.array([len(sl)])))
        parts.append(varint(np.diff(np.concatenate(([0], sl)))))
        for c in range(3):
            parts.append(varint(vel[c][t, sl] - der[c][t, sl]))
    if parts:
        tot += len(zc.compress(b"".join(parts)))
print(f"  the true velocity at an exact point every 30 ticks: zstd19 {tot / 1e6:.2f} MB for the battle")
