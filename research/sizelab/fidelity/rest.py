#!/usr/bin/env python3
"""rest.py -- the unit stream's other fields, exact, as columns: the denominators the motion
curves are read against.

    rest.py <datadir>

Groups (offsets in fields.py and tagpu_recfmt.h):
  mover    vx, vy, vz, speed, w24, claim, moved (predicted GameTime - 1), layer
  derived  footprint cell fx, fz, sight cell sx, sz, sight height sh, bucket, water
  rest     every other dword of the image, as dwords
Each group, per block of 300 iterations: per iteration the changed units' slot gaps, a mask of
the group's changed fields per unit, then each field's changes (zigzag varint of the difference
from the unit's previous value) as a column. Compressed per block as motion.py does.
"""
import lzma
import sys
import zlib
from pathlib import Path

import numpy as np
import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
from motion import varint, BLOCK  # noqa: E402

d = Path(sys.argv[1])
L = np.load(d / "live.npy").astype(bool)
T, S = L.shape
both = np.zeros_like(L)
both[1:] = L[1:] & L[:-1]
gt = np.arange(1, T + 1, dtype=np.int64)


def F(n):
    return np.load(d / f"f_{n}.npy").astype(np.int64)


def group_size(fields, preds=None):
    """fields: list of [T, S] int64 arrays. Returns compressed sizes over blocks."""
    preds = preds or {}
    raw = defl = zst = 0
    zc = zstandard.ZstdCompressor(level=19)
    nchg = 0
    tail = 0
    for b0 in range(0, T, BLOCK):
        b1 = min(T, b0 + BLOCK)
        parts_t, parts_s, parts_m = [], [], []
        cols = [[] for _ in fields]
        for t in range(max(b0, 1), b1):
            lb = both[t]
            chg = np.zeros((len(fields), S), dtype=bool)
            for i, a in enumerate(fields):
                chg[i] = (a[t] != a[t - 1]) & lb
            anyc = chg.any(0)
            sl = np.nonzero(anyc)[0]
            if not len(sl):
                continue
            nchg += len(sl)
            parts_t.append(t - b0)
            parts_s.append(np.diff(np.concatenate(([0], sl))))
            m = np.zeros(len(sl), dtype=np.int64)
            for i in range(len(fields)):
                m |= chg[i, sl].astype(np.int64) << i
            parts_m.append(m)
            for i, a in enumerate(fields):
                w = sl[chg[i, sl]]
                if i in preds:
                    cols[i].append(a[t, w] - preds[i](t, w))
                else:
                    cols[i].append(a[t, w] - a[t - 1, w])
        if not parts_t:
            continue
        data = varint(np.array(parts_t)) + varint(np.array([len(s) for s in parts_s])) + \
            varint(np.concatenate(parts_s)) + varint(np.concatenate(parts_m))
        for c in cols:
            if c:
                data += varint(np.concatenate(c))
        raw += len(data)
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        defl += len(c.compress(data) + c.flush())
        zb = len(zc.compress(data))
        zst += zb
        if 4500 <= b0 < 7200:
            tail += zb
    return {"unit_changes": nchg, "raw": raw, "deflate2k": defl, "zstd19": zst, "tail_mb_h": tail / 3 / 1e6 * 120}


def show(name, r):
    print(f"{name:<34} changes {r['unit_changes']:>9,}  raw {r['raw'] / 1e6:7.2f} MB  "
          f"deflate2k {r['deflate2k'] / 1e6:6.2f}  zstd19 {r['zstd19'] / 1e6:6.2f}  tail {r['tail_mb_h']:5.1f} MB/h", flush=True)


moved = F("moved")
show("mover: velocity, speed, w24", group_size([F("vx"), F("vy"), F("vz"), F("spd"), F("w24")]))
show("mover: claim, moved (pred GT-1), layer",
     group_size([F("claim"), moved, F("layer")],
                {1: lambda t, w: np.full(len(w), gt[t] - 1)}))
show("derived: cells, sight, bucket, water",
     group_size([F("fx"), F("fz"), F("sx"), F("sz"), F("sh"), F("bucket"), F("water")]))
u = np.load(d / "units.npy", mmap_mode="r")
skip = set(range(0x64 // 4, 0x80 // 4)) | {0x7C // 4, 0xF8 // 4} | set(range(0x118 // 4, 0x148 // 4))
keep = [i for i in range(u.shape[2]) if i not in skip]
rest = [np.asarray(u[:, :, i]).astype(np.int64) for i in keep]
show(f"rest: {len(keep)} other dwords", group_size(rest))
