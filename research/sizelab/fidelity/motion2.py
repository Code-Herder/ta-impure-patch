#!/usr/bin/env python3
"""motion2.py -- the second round: whole world units by TRUNCATION (the high word kept exact,
as every engine reader of a unit's position on a puppet reads it), the derived state those
readers compute (footprint cell, sight cell), exact resync points for takeover, two-sided
samples around every change of a derived field, and the size by window of game time.

    motion2.py <datadir> <out.json>
"""
import json
import sys
from pathlib import Path

import numba as nb
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402

M.WITH_LZMA = False
W = M.W
SNAP = M.SNAP


@nb.njit(cache=True)
def quant(v, q, trunc):
    if q == 1:
        return v
    if trunc:
        return (v // q) * q
    return ((v + q // 2) // q) * q


@nb.njit(cache=True)
def interp(va, vb, t, a, h):
    num = (vb - va) * (t - a)
    return va + (num + h // 2) // h if num >= 0 else va - ((-num + h // 2) // h)


@nb.njit(cache=True)
def sval(V, i, col, q, trunc, exact):
    return V[i, col] if exact[i] else quant(V[i, col], q, trunc)


@nb.njit(cache=True)
def fixed_keep(offs, r0, k, resync, keep, exact):
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        for i in range(o0, o1):
            g = r0[si] + (i - o0) + 1
            if i == o0 or i == o1 - 1 or g % k == 0 or g % SNAP == 1:
                keep[i] = 1
            if g % SNAP == 1 or (resync > 0 and g % resync == 1) or i == o0:
                keep[i] = 1
                exact[i] = 1


@nb.njit(cache=True)
def tol_keep(V, offs, r0, cols, q, trunc, eps, maxgap, resync, forced, keep, exact):
    """Greedy error-bounded sampling of the field group `cols` (a pair: Euclidean), quantized
    samples, exact at a life's first tick, the snapshots and the resync ticks; `forced` ticks
    are always kept."""
    nc = len(cols)
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        for i in range(o0, o1):
            g = r0[si] + (i - o0) + 1
            if i == o0 or g % SNAP == 1 or (resync > 0 and g % resync == 1):
                exact[i] = 1
        a = o0
        keep[a] = 1
        while a < o1 - 1:
            best = a + 1
            b = a + 1
            while b < o1 and b - a <= maxgap:
                ok = True
                h = b - a
                for t in range(a + 1, b):
                    e2 = 0.0
                    for c in range(nc):
                        col = cols[c]
                        va = sval(V, a, col, q[c], trunc, exact)
                        vb = sval(V, b, col, q[c], trunc, exact)
                        e = float(interp(va, vb, t, a, h) - V[t, col])
                        e2 += e * e
                    if e2 > eps * eps:
                        ok = False
                        break
                if not ok:
                    break
                best = b
                if exact[b] or forced[b]:
                    break
                b += 1
            keep[best] = 1
            a = best
        keep[o1 - 1] = 1


@nb.njit(cache=True)
def rebuild(V, offs, keep, exact, col, q, trunc, out):
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        last = -1
        for i in range(o0, o1):
            if keep[i]:
                vb = sval(V, i, col, q, trunc, exact)
                out[i] = vb
                if last >= 0 and i - last > 1:
                    va = sval(V, last, col, q, trunc, exact)
                    for t in range(last + 1, i):
                        out[t] = interp(va, vb, t, last, i - last)
                last = i


def main():
    d = Path(sys.argv[1])
    V, offs, slot, r0, T = M.load(d)
    n = len(V)
    seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
    rows = r0[seg_of] + (np.arange(n) - offs[seg_of])
    sl = slot[seg_of]
    fw = np.load(d / "f_fw.npy")[rows, sl].astype(np.int64)
    fh = np.load(d / "f_fh.npy")[rows, sl].astype(np.int64)
    derived = {nm: np.load(d / f"f_{nm}.npy")[rows, sl].astype(np.int64)
               for nm in ("fx", "fz", "sx", "sz", "sh", "bucket", "water")}
    same = np.zeros(n, dtype=bool)
    same[1:] = seg_of[1:] == seg_of[:-1]
    ch = np.zeros(n, dtype=bool)
    dpos = ((V[1:, 0] != V[:-1, 0]) | (V[1:, 1] != V[:-1, 1])) & same[1:]
    ch[1:] |= dpos
    ch[:-1] |= dpos
    # two-sided forced samples around a change of a derived field
    def two_sided(names):
        f = np.zeros(n, dtype=np.uint8)
        for nm in names:
            a = derived[nm]
            c = np.zeros(n, dtype=bool)
            c[1:] = (a[1:] != a[:-1]) & same[1:]
            f[c] = 1
            f[np.nonzero(c)[0] - 1] = 1
        return f
    forced_cells = two_sided(("fx", "fz"))
    forced_all = two_sided(("fx", "fz", "sx", "sz", "sh", "bucket", "water"))
    none = np.zeros(n, dtype=np.uint8)
    print(f"{n:,} unit-ticks; footprint-cell changes force {forced_cells.sum():,} samples, "
          f"every derived field {forced_all.sum():,}", flush=True)

    results = []

    def stats(R):
        o = M.err_stats(V, R, ch)
        hx = (R[:, 0] >> 16) != (V[:, 0] >> 16)
        hz = (R[:, 1] >> 16) != (V[:, 1] >> 16)
        cx = ((R[:, 0] - (fw << 19) + 0x80000) >> 20) != derived["fx"]
        cz = ((R[:, 1] - (fh << 19) + 0x80000) >> 20) != derived["fz"]
        sx = (R[:, 0] >> 21) != (V[:, 0] >> 21)
        ha = (R[:, 2] >> 16) != (V[:, 2] >> 16)
        for lab, m in (("all", np.ones(n, bool)), ("moving", ch)):
            o[lab]["highword_xz_differs"] = float((hx | hz)[m].mean())
            o[lab]["footprint_cell_differs"] = float((cx | cz)[m].mean())
            o[lab]["sight_x_cell_differs"] = float(sx[m].mean())
            o[lab]["alt_highword_differs"] = float(ha[m].mean())
        return o

    def window_sizes(szs):
        win = {}
        for s in szs:
            for b, v in s["per_block_zstd"].items():
                w = (int(b) * M.BLOCK) // SNAP
                win[w] = win.get(w, 0) + v
        return {int(k): v for k, v in sorted(win.items())}

    def run(label, keeps, exacts, qs, trunc, extra):
        R = np.empty_like(V)
        for c in range(4):
            rebuild(V, offs, keeps[c], exacts[c], c, qs[c], trunc if c < 3 else 0, R[:, c])
        st = stats(R)
        groups, seen = [], {}
        for c in range(4):
            key = id(keeps[c])
            if key not in seen:
                seen[key] = len(groups)
                groups.append((keeps[c], []))
            groups[seen[key]][1].append((c, qs[c]))
        fixed = extra.get("kind") == "fixed"
        # the samples as the stream carries them: floor(v / q) where truncated (the size code
        # rounds, so it is given v - q/2), the heading rounded
        Vs = V.copy()
        if trunc:
            for c in range(3):
                if qs[c] > 1:
                    Vs[:, c] -= qs[c] // 2
        main = M.stream_size(Vs, offs, slot, r0, [g[0] for g in groups],
                             [g[1] for g in groups], fixed, 1 if not fixed else 2)
        szs = [main]
        exact_any = np.zeros(n, dtype=np.uint8)
        for e in exacts:
            exact_any |= e
        lowbits = None
        if any(q > 1 for q in qs):
            # the exact points' low bits (what the quantized sample lacks), for units whose low
            # bits changed since their previous exact point; a life's first tick is its create
            ex = exact_any.astype(bool)
            ex[offs[:-1]] = False
            if ex.any():
                Vf = np.zeros_like(V)
                for c in range(4):
                    q = qs[c]
                    if q > 1:
                        qv = (V[:, c] // q) * q if (trunc and c < 3) else ((V[:, c] + q // 2) // q) * q
                        Vf[:, c] = V[:, c] - qv
                lowbits = M.stream_size(Vf, offs, slot, r0, [ex.astype(np.uint8)],
                                        [[(c, 1) for c in range(4) if qs[c] > 1]], False, 1)
                szs.append(lowbits)
        tot = sum(s["zstd19"] for s in szs)
        totd = sum(s["deflate2k"] for s in szs)
        r = {"label": label, "errors": st, "size_main": {k: v for k, v in main.items()
                                                           if k != "per_block_zstd"},
             "size_exact_points": None if lowbits is None else
             {k: v for k, v in lowbits.items() if k != "per_block_zstd"},
             "zstd19": tot, "deflate2k": totd, "windows_zstd": window_sizes(szs),
             "samples": int(sum(g[0].sum() for g in groups))}
        r.update(extra)
        results.append(r)
        e = st["moving"]
        print(f"{label:<52} pos p50 {e['pos_wu'][50]:.3f} p99 {e['pos_wu'][99]:.3f} max "
              f"{e['pos_wu']['max']:.2f} | hd p99 {e['hd_deg'][99]:.2f} max {e['hd_deg']['max']:.1f}"
              f" | hw {100 * e['highword_xz_differs']:.1f}% cell {100 * e['footprint_cell_differs']:.2f}%"
              f" sight {100 * e['sight_x_cell_differs']:.2f}% alt-hw {100 * e['alt_highword_differs']:.2f}%"
              f" | zstd {tot / 1e6:.2f} MB defl {totd / 1e6:.2f}"
              + (f" (exact points {lowbits['zstd19'] / 1e6:.2f})" if lowbits else ""), flush=True)

    # the lossless reference, and its windows
    for k in (1, 3):
        keep = np.zeros(n, np.uint8)
        exact = np.zeros(n, np.uint8)
        M.fixed_samples(offs, r0, k, keep)
        if k == 1:
            run("30 Hz exact (lossless reference)", [keep] * 4, [exact] * 4, [1, 1, 1, 1], 0,
                {"kind": "fixed", "k": 1})
    # the high word exact: truncation at fixed rates, heading u8 (rounded)
    for k in (1, 2, 3, 6):
        for resync in (0, 30):
            keep = np.zeros(n, np.uint8)
            exact = np.zeros(n, np.uint8)
            fixed_keep(offs, r0, k, resync, keep, exact)
            run(f"{30 // k} Hz whole wu TRUNC, hd u8" + (", exact every 1 s" if resync else ""),
                [keep] * 4, [exact] * 4, [W, W, W, 256], 1,
                {"kind": "fixed", "k": k, "resync": resync, "trunc": 1})
    # 10 Hz rounded, for the contrast with truncation
    keep = np.zeros(n, np.uint8)
    exact = np.zeros(n, np.uint8)
    fixed_keep(offs, r0, 3, 0, keep, exact)
    run("10 Hz whole wu ROUND, hd u8", [keep] * 4, [exact] * 4, [W, W, W, 256], 0,
        {"kind": "fixed", "k": 3, "resync": 0, "trunc": 0})
    # error-bounded, whole wu truncated, with and without forced two-sided samples
    for eps, epsh in ((1.0, 2.0), (2.0, 4.0)):
        for flab, forced in (("", none), (", cells two-sided", forced_cells),
                             (", every derived field two-sided", forced_all)):
            for resync in (0, 30):
                kp, ep = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
                tol_keep(V, offs, r0, np.array([0, 1]), np.array([W, W]), 1, eps * W, 30,
                         resync, forced, kp, ep)
                ka, ea = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
                tol_keep(V, offs, r0, np.array([2]), np.array([W]), 1, eps * W, 30, resync,
                         forced, ka, ea)
                kh, eh = np.zeros(n, np.uint8), np.zeros(n, np.uint8)
                tol_keep(V, offs, r0, np.array([3]), np.array([256]), 0, epsh * 65536 / 360, 30,
                         resync, none, kh, eh)
                run(f"tol {eps} wu {epsh} deg TRUNC{flab}" + (", exact 1 s" if resync else ""),
                    [kp, kp, ka, kh], [ep, ep, ea, eh], [W, W, W, 256], 1,
                    {"kind": "tolerance", "eps": eps, "eps_hd": epsh, "resync": resync,
                     "forced": flab})
    Path(sys.argv[2]).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
