#!/usr/bin/env python3
"""motion4.py -- sub-unit samples by TRUNCATION (low bits dropped, the high word kept), with
and without two-sided samples around the derived fields' changes; and where rounding lands.

    motion4.py <datadir>
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402
import motion2 as M2  # noqa: E402
import motion3 as M3  # noqa: E402

M.WITH_LZMA = False
W = M.W

d = Path(sys.argv[1])
V, offs, slot, r0, T = M.load(d)
n = len(V)
seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
same = np.zeros(n, dtype=bool)
same[1:] = seg_of[1:] == seg_of[:-1]
mv = np.zeros(n, dtype=bool)
mv[1:] = ((V[1:, 0] != V[:-1, 0]) | (V[1:, 1] != V[:-1, 1])) & same[1:]
fr = np.concatenate(((V[mv, 0] & 0xFFFF), (V[mv, 1] & 0xFFFF)))
print("moving ticks' x/z fractions: within 1/32 wu below the next whole unit "
      f"{100 * np.mean(fr >= 0xFFFF - 2048):.1f} %, exactly 0xFFFF {100 * np.mean(fr == 0xFFFF):.1f} %,"
      f" zero {100 * np.mean(fr == 0):.1f} %", flush=True)
sys.argv = [sys.argv[0], str(d)]


def run():
    rows = r0[seg_of] + (np.arange(n) - offs[seg_of])
    sl = slot[seg_of]
    derived = {nm: np.load(d / f"f_{nm}.npy")[rows, sl].astype(np.int64)
               for nm in ("fx", "fz", "sx", "sz", "sh", "bucket", "water")}
    fw = np.load(d / "f_fw.npy")[rows, sl].astype(np.int64)
    fh = np.load(d / "f_fh.npy")[rows, sl].astype(np.int64)
    forced = np.zeros(n, dtype=np.uint8)
    for a in derived.values():
        c = np.zeros(n, dtype=bool)
        c[1:] = (a[1:] != a[:-1]) & same[1:]
        forced[c] = 1
        forced[np.nonzero(c)[0] - 1] = 1
    none = np.zeros(n, dtype=np.uint8)
    for eps, epsh, q, frc, lab in ((0.25, 1.0, 4096, none, "tol 0.25 wu 1 deg, 1/16 TRUNC"),
                                   (0.25, 1.0, 4096, forced, "tol 0.25 wu 1 deg, 1/16 TRUNC, derived 2-sided"),
                                   (0.5, 1.0, 4096, forced, "tol 0.5 wu 1 deg, 1/16 TRUNC, derived 2-sided"),
                                   (0.5, 2.0, 4096, forced, "tol 0.5 wu 2 deg, 1/16 TRUNC, derived 2-sided")):
        kp, ep = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([0, 1]), np.array([q, q]), 1, eps * W, 30, 0, frc, kp, ep)
        ka, ea = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([2]), np.array([q]), 1, eps * W, 30, 0, frc, ka, ea)
        kh, eh = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([3]), np.array([1]), 0, epsh * 65536 / 360, 30, 0, none,
                    kh, eh)
        R = np.empty_like(V)
        for c, (k_, e_, qq) in enumerate(((kp, ep, q), (kp, ep, q), (ka, ea, q), (kh, eh, 1))):
            M2.rebuild(V, offs, k_, e_, c, qq, 1 if c < 3 else 0, R[:, c])
        Vs = V.copy()
        Vs[:, :3] -= q // 2
        s = M.stream_size(Vs, offs, slot, r0, [kp, ka, kh], [[(0, q), (1, q)], [(2, q)], [(3, 1)]],
                          False, 1)
        tail = sum(v for b, v in s["per_block_zstd"].items() if 4500 <= b * M.BLOCK < 7200)
        ex = np.hypot((R[:, 0] - V[:, 0]) / W, (R[:, 1] - V[:, 1]) / W)[mv]
        j = np.hypot(np.diff((R[:, 0] - V[:, 0]) / W), np.diff((R[:, 1] - V[:, 1]) / W))[mv[1:]]
        cell = ((((R[:, 0] - (fw << 19) + 0x80000) >> 20) != derived["fx"]) |
                (((R[:, 1] - (fh << 19) + 0x80000) >> 20) != derived["fz"]))[mv].mean()
        sx = ((R[:, 0] >> 21) != (V[:, 0] >> 21))[mv].mean()
        hw = (((R[:, 0] >> 16) != (V[:, 0] >> 16)) | ((R[:, 1] >> 16) != (V[:, 1] >> 16)))[mv].mean()
        hd = np.abs(R[:, 3] - V[:, 3])[mv] * 360 / 65536
        print(f"{lab:<48} err p50 {np.percentile(ex, 50):.2f} p99 {np.percentile(ex, 99):.2f} max "
              f"{ex.max():.2f} | jitter p99 {np.percentile(j, 99):.3f} | hd p99 {np.percentile(hd, 99):.2f}"
              f" max {hd.max():.2f} | high word differs {100 * hw:.1f}% cell {100 * cell:.2f}% sight-x "
              f"{100 * sx:.2f}% | zstd {s['zstd19'] / 1e6:.2f} MB, deflate2k {s['deflate2k'] / 1e6:.2f},"
              f" tail {tail / 3 / 1e6 * 120:.1f} MB/h", flush=True)


run()
