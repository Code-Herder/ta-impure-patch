#!/usr/bin/env python3
"""motion5.py -- every tick, the high word exact and k bits of the fraction kept by truncation
(k = 0, 2, 4), the heading at 8 or 16 bits: size, error, jitter, derived state.

    motion5.py <datadir>
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402
import motion2 as M2  # noqa: E402

M.WITH_LZMA = False
W = M.W
d = Path(sys.argv[1])
V, offs, slot, r0, T = M.load(d)
n = len(V)
seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
rows = r0[seg_of] + (np.arange(n) - offs[seg_of])
sl = slot[seg_of]
same = np.zeros(n, dtype=bool)
same[1:] = seg_of[1:] == seg_of[:-1]
mv = np.zeros(n, dtype=bool)
mv[1:] = ((V[1:, 0] != V[:-1, 0]) | (V[1:, 1] != V[:-1, 1])) & same[1:]
fx = np.load(d / "f_fx.npy")[rows, sl].astype(np.int64)
fz = np.load(d / "f_fz.npy")[rows, sl].astype(np.int64)
fw = np.load(d / "f_fw.npy")[rows, sl].astype(np.int64)
fh = np.load(d / "f_fh.npy")[rows, sl].astype(np.int64)
keep = np.zeros(n, np.uint8)
exact = np.zeros(n, np.uint8)
M2.fixed_keep(offs, r0, 1, 0, keep, exact)
for bits in (0, 2, 4):
    for qh in (256, 1):
        q = W >> bits
        R = np.empty_like(V)
        for c in range(4):
            M2.rebuild(V, offs, keep, exact, c, q if c < 3 else qh, 1 if c < 3 else 0, R[:, c])
        Vs = V.copy()
        Vs[:, :3] -= q // 2
        s = M.stream_size(Vs, offs, slot, r0, [keep], [[(0, q), (1, q), (2, q), (3, qh)]], True, 2)
        tail = sum(v for b, v in s["per_block_zstd"].items() if 4500 <= b * M.BLOCK < 7200)
        ex = (R[:, 0] - V[:, 0]) / W
        ez = (R[:, 1] - V[:, 1]) / W
        e = np.hypot(ex, ez)[mv]
        j = np.hypot(np.diff(ex), np.diff(ez))[mv[1:]]
        cell = ((((R[:, 0] - (fw << 19) + 0x80000) >> 20) != fx) |
                (((R[:, 1] - (fh << 19) + 0x80000) >> 20) != fz))[mv].mean()
        hd = np.abs(R[:, 3] - V[:, 3])[mv] * 360 / 65536
        print(f"30 Hz, {bits} fraction bits, heading {16 if qh == 1 else 8}-bit: err p50 "
              f"{np.percentile(e, 50):.3f} max {e.max():.3f} wu | jitter p99 {np.percentile(j, 99):.3f}"
              f" | hd max {hd.max():.2f} | cell differs {100 * cell:.2f}% | zstd {s['zstd19'] / 1e6:.2f}"
              f" MB, deflate2k {s['deflate2k'] / 1e6:.2f}, tail {tail / 3 / 1e6 * 120:.1f} MB/h",
              flush=True)
