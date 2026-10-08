#!/usr/bin/env python3
"""motion3.py -- the third round: what the eye sees (the error's change from one tick to the
next, jitter, against its level, offset), the cost split by field, and sub-unit samples with
the derived state kept exact by two-sided samples.

    motion3.py <datadir>
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402
import motion2 as M2  # noqa: E402

M.WITH_LZMA = False
W = M.W


def main():
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
    derived = {nm: np.load(d / f"f_{nm}.npy")[rows, sl].astype(np.int64)
               for nm in ("fx", "fz", "sx", "sz", "sh", "bucket", "water")}
    forced = np.zeros(n, dtype=np.uint8)
    for a in derived.values():
        c = np.zeros(n, dtype=bool)
        c[1:] = (a[1:] != a[:-1]) & same[1:]
        forced[c] = 1
        forced[np.nonzero(c)[0] - 1] = 1
    none = np.zeros(n, dtype=np.uint8)
    fw = np.load(d / "f_fw.npy")[rows, sl].astype(np.int64)
    fh = np.load(d / "f_fh.npy")[rows, sl].astype(np.int64)

    def jitter(R):
        """per moving tick (the unit moved since the tick before): the change of the horizontal
        error vector since the tick before, in wu -- what shows as a stutter; and the error
        itself"""
        ex = (R[:, 0] - V[:, 0]) / W
        ez = (R[:, 1] - V[:, 1]) / W
        j = np.hypot(np.diff(ex), np.diff(ez))
        j = j[mv[1:]]
        e = np.hypot(ex, ez)[mv]
        hj = np.abs(np.diff(R[:, 3] - V[:, 3]))[mv[1:]] * 360 / 65536
        cells = (((R[:, 0] - (fw << 19) + 0x80000) >> 20) != derived["fx"]) | \
            (((R[:, 1] - (fh << 19) + 0x80000) >> 20) != derived["fz"])
        return (np.percentile(e, [50, 99]).tolist() + [e.max()],
                np.percentile(j, [50, 90, 99]).tolist() + [j.max()],
                np.percentile(hj, [99]).tolist() + [hj.max()], cells[mv].mean())

    def show(label, R, size):
        e, j, h, c = jitter(R)
        print(f"{label:<46} err p50 {e[0]:.2f} p99 {e[1]:.2f} max {e[2]:.2f} | jitter p50 "
              f"{j[0]:.3f} p90 {j[1]:.3f} p99 {j[2]:.3f} max {j[3]:.2f} | hd jitter p99 {h[0]:.2f}"
              f" | cell differs {100 * c:.2f}% | {size}", flush=True)

    def fixed(k, qs, trunc, resync=0):
        keep, exact = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.fixed_keep(offs, r0, k, resync, keep, exact)
        R = np.empty_like(V)
        for c in range(4):
            M2.rebuild(V, offs, keep, exact, c, qs[c], trunc if c < 3 else 0, R[:, c])
        return R, keep

    def tol(eps, epsh, q, trunc, frc, qh=1):
        kp, ep = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([0, 1]), np.array([q, q]), trunc, eps * W, 30, 0, frc,
                    kp, ep)
        ka, ea = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([2]), np.array([q]), trunc, eps * W, 30, 0, frc, ka, ea)
        kh, eh = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
        M2.tol_keep(V, offs, r0, np.array([3]), np.array([qh]), 0, epsh * 65536 / 360, 30, 0,
                    none, kh, eh)
        R = np.empty_like(V)
        for c, (k_, e_, qq) in enumerate(((kp, ep, q), (kp, ep, q), (ka, ea, q), (kh, eh, qh))):
            M2.rebuild(V, offs, k_, e_, c, qq, trunc if c < 3 else 0, R[:, c])
        Vs = V.copy()
        if trunc and q > 1:
            Vs[:, :3] -= q // 2
        s = M.stream_size(Vs, offs, slot, r0, [kp, ka, kh], [[(0, q), (1, q)], [(2, q)], [(3, qh)]],
                          False, 1)
        return R, f"zstd {s['zstd19'] / 1e6:.2f} MB"

    R, _ = fixed(1, [1, 1, 1, 1], 0)
    show("30 Hz exact", R, "zstd 3.52 MB (motion.out)")
    R, _ = fixed(1, [W, W, W, 256], 1)
    show("30 Hz whole wu trunc, hd u8", R, "zstd 1.09 MB (motion2.out)")
    R, _ = fixed(1, [4096, 4096, 4096, 1], 0)
    show("30 Hz 1/16 wu", R, "zstd 1.90 MB (motion.out)")
    R, _ = fixed(3, [W, W, W, 256], 1)
    show("10 Hz whole wu trunc, hd u8", R, "zstd 0.62 MB (motion2.out)")
    R, _ = fixed(3, [4096, 4096, 4096, 1], 0)
    show("10 Hz 1/16 wu", R, "zstd 1.15 MB (motion.out)")
    R, _ = fixed(3, [1, 1, 1, 1], 0)
    show("10 Hz 16.16", R, "zstd 2.08 MB (motion.out)")
    for eps, epsh, q, tr, frc, lab in ((0.25, 1.0, 4096, 0, none, "tol 0.25 wu 1 deg, 1/16"),
                                       (0.25, 1.0, 4096, 0, forced, "tol 0.25 wu 1 deg, 1/16, derived 2-sided"),
                                       (0.5, 1.0, 4096, 0, forced, "tol 0.5 wu 1 deg, 1/16, derived 2-sided"),
                                       (1.0, 2.0, W, 1, forced, "tol 1 wu 2 deg, trunc, derived 2-sided")):
        R, s = tol(eps, epsh, q, tr, frc)
        show(lab, R, s)
    # the cost split: each field alone, lossless 30 Hz and reduced
    keep = np.zeros(n, np.uint8)
    M.fixed_samples(offs, r0, 1, keep)
    for lab, cols in (("x, z", [(0, 1), (1, 1)]), ("altitude", [(2, 1)]), ("heading u16", [(3, 1)]),
                      ("heading u8", [(3, 256)])):
        s = M.stream_size(V, offs, slot, r0, [keep], [cols], True, 2)
        print(f"30 Hz {lab:<14} alone: zstd {s['zstd19'] / 1e6:.2f} MB, deflate2k "
              f"{s['deflate2k'] / 1e6:.2f}", flush=True)


if __name__ == "__main__":
    main()
