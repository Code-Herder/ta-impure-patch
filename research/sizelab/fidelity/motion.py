#!/usr/bin/env python3
"""motion.py -- the battle's unit motion, sampled, quantized and reconstructed: the error at
every tick against the recording, and the size of the samples as a column stream.

    motion.py <datadir> <out.json>

Each unit's life (a run of live iterations of one slot) is one series of
  x, z, alt  (16.16 world units)   and   hd (the heading, unwrapped from u16, 65536 = 360 deg).
A configuration picks the samples a recording would keep, quantizes them, and the decoder's
interpolation rebuilds every tick in between; the error is the rebuilt value less the
recording's, at every live tick. Every configuration keeps a sample at a life's first and last
tick and at every snapshot tick (GameTime 900k + 1), so seek and takeover points are exact.

Size: the kept samples of a unit whose value changed since its previous kept sample, written
as columns per block of BLOCK iterations (the count of units per iteration, their slot gaps, the
ticks to the next sample where the rate is not fixed, then each field's change in quantized
units, zigzag varints), compressed per block with the writer's deflate (2 KB window), zstd 19
and lzma. A life's first sample is the create's image, which the recording carries anyway, and
is not counted.
"""
import json
import lzma
import sys
import zlib
from pathlib import Path

import numba as nb
import numpy as np
import zstandard

BLOCK = 300          # iterations a block: 10 s, three to a snapshot interval
WITH_LZMA = True
SNAP = 900           # the snapshot interval; snapshots at GameTime 900k + 1
W = 65536            # one world unit in 16.16


# ---------------------------------------------------------------- the series
def load(d):
    L = np.load(d / "live.npy").astype(bool)
    T, S = L.shape
    f = {n: np.load(d / f"f_{n}.npy") for n in ("x", "z", "alt", "hd")}
    segs = []
    for s in range(S):
        col = L[:, s].astype(np.int8)
        if not col.any():
            continue
        dd = np.diff(np.concatenate(([0], col, [0])))
        for a, b in zip(np.where(dd == 1)[0], np.where(dd == -1)[0]):
            segs.append((s, a, b))
    segs.sort(key=lambda r: (r[0], r[1]))
    n = sum(b - a for _, a, b in segs)
    V = np.empty((n, 4), dtype=np.int64)
    offs = np.empty(len(segs) + 1, dtype=np.int64)
    slot = np.empty(len(segs), dtype=np.int64)
    r0 = np.empty(len(segs), dtype=np.int64)
    o = 0
    for i, (s, a, b) in enumerate(segs):
        offs[i], slot[i], r0[i] = o, s, a
        m = b - a
        V[o:o + m, 0] = f["x"][a:b, s]
        V[o:o + m, 1] = f["z"][a:b, s]
        V[o:o + m, 2] = f["alt"][a:b, s]
        h = f["hd"][a:b, s].astype(np.int64)
        dh = ((np.diff(h) + 32768) % 65536) - 32768
        V[o:o + m, 3] = np.concatenate(([h[0]], h[0] + np.cumsum(dh)))
        o += m
    offs[-1] = o
    return V, offs, slot, r0, T


# ---------------------------------------------------------------- samplers
@nb.njit(cache=True)
def qround(v, q):
    if q == 1:
        return v
    return ((v + q // 2) // q) * q


@nb.njit(cache=True)
def fixed_samples(offs, r0, k, keep):
    """keep[i] = 1 where a fixed-rate recording keeps a sample: GameTime % k == 0, a life's
    ends, the snapshot ticks."""
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        for i in range(o0, o1):
            g = r0[si] + (i - o0) + 1
            if i == o0 or i == o1 - 1 or g % k == 0 or g % SNAP == 1:
                keep[i] = 1


@nb.njit(cache=True)
def tolerance_samples(V, offs, r0, cols, q, eps2, eps_scalar, maxgap, keep, forced):
    """Greedy error-bounded sampling over the field group `cols` (one or two columns; two are
    tested as a Euclidean distance): from each kept sample, the furthest next tick such that
    linear interpolation between the two QUANTIZED samples misses no tick in between by more
    than eps (eps2 squared for a pair, eps_scalar for one). Forced samples (a life's ends, the
    snapshot ticks, and `forced`) are always kept."""
    nc = len(cols)
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        a = o0
        keep[a] = 1
        while a < o1 - 1:
            best = a + 1
            b = a + 1
            while b < o1 and b - a <= maxgap:
                # quantized endpoints
                ok = True
                h = b - a
                for t in range(a + 1, b):
                    e2 = 0.0
                    for c in range(nc):
                        col = cols[c]
                        va = qround(V[a, col], q[c])
                        vb = qround(V[b, col], q[c])
                        num = (vb - va) * (t - a)
                        r = va + (num + h // 2) // h if num >= 0 else va - ((-num + h // 2) // h)
                        e = float(r - V[t, col])
                        e2 += e * e
                    if nc == 2:
                        if e2 > eps2:
                            ok = False
                            break
                    else:
                        if e2 > eps_scalar * eps_scalar:
                            ok = False
                            break
                if not ok:
                    break
                best = b
                g = r0[si] + (b - o0) + 1
                if g % SNAP == 1 or forced[b]:
                    break
                b += 1
            keep[best] = 1
            a = best
        keep[o1 - 1] = 1


@nb.njit(cache=True)
def rebuild(V, offs, keep, col, q, mode, out):
    """out[:] = the decoder's value of V[:, col] at every tick: the quantized kept samples,
    and between two of them linear (mode 1) or Catmull-Rom / cubic Hermite with finite-difference
    tangents (mode 2), or the last sample held (mode 0)."""
    for si in range(len(offs) - 1):
        o0, o1 = offs[si], offs[si + 1]
        # the kept indices of this life
        idx = np.empty(o1 - o0, dtype=np.int64)
        m = 0
        for i in range(o0, o1):
            if keep[i]:
                idx[m] = i
                m += 1
        for j in range(m):
            out[idx[j]] = qround(V[idx[j], col], q)
        for j in range(m - 1):
            a, b = idx[j], idx[j + 1]
            va, vb = qround(V[a, col], q), qround(V[b, col], q)
            h = b - a
            if h == 1:
                continue
            if mode == 0:
                for t in range(a + 1, b):
                    out[t] = va
            elif mode == 1:
                for t in range(a + 1, b):
                    num = (vb - va) * (t - a)
                    out[t] = va + (num + h // 2) // h if num >= 0 else va - ((-num + h // 2) // h)
            else:
                p = idx[j - 1] if j > 0 else a
                n = idx[j + 2] if j + 2 < m else b
                vp = qround(V[p, col], q) if p != a else va
                vn = qround(V[n, col], q) if n != b else vb
                ma = (vb - vp) / (b - p) if p != a else (vb - va) / h
                mb = (vn - va) / (n - a) if n != b else (vb - va) / h
                for t in range(a + 1, b):
                    u = (t - a) / h
                    u2 = u * u
                    u3 = u2 * u
                    v = (2 * u3 - 3 * u2 + 1) * va + (u3 - 2 * u2 + u) * h * ma + \
                        (-2 * u3 + 3 * u2) * vb + (u3 - u2) * h * mb
                    out[t] = np.int64(np.floor(v + 0.5))


# ---------------------------------------------------------------- the size
def varint(vals):
    """Zigzag LEB128 of an int64 array, as bytes."""
    v = vals.astype(np.int64)
    u = ((v << 1) ^ (v >> 63)).astype(np.uint64)
    nb_ = np.ones(len(u), dtype=np.int64)
    t = u >> np.uint64(7)
    while True:
        more = t > 0
        if not more.any():
            break
        nb_ += more
        t = t >> np.uint64(7)
    mx = int(nb_.max()) if len(nb_) else 1
    grid = np.zeros((len(u), mx), dtype=np.uint8)
    t = u.copy()
    for j in range(mx):
        grid[:, j] = (t & np.uint64(0x7F)).astype(np.uint8)
        t = t >> np.uint64(7)
    cont = np.arange(mx)[None, :] < (nb_[:, None] - 1)
    grid |= (cont.astype(np.uint8) << 7)
    mask = np.arange(mx)[None, :] < nb_[:, None]
    return grid[mask].tobytes()


def stream_size(V, offs, slot, r0, keeps, qs, fixed_k, predictor=1):
    """The kept samples of each field group as a column stream; keeps[g] the group's keep mask,
    qs[g] its columns' (col, quantum). Fixed rate: a kept sample is written when its quantized
    value differs from the unit's previous kept one; a grid tick a unit is not written at holds
    its value, so the decoder knows every grid sample. Tolerance: a kept sample is written when
    it differs from the previous kept one or the next kept one differs from it (the two ends of
    a hold), with its ticks since the previous written one. The prediction is the previous kept
    sample (1) or the line through the two before it (2). Returns the compressed sizes summed
    over blocks, and the raw varint size."""
    nseg = len(offs) - 1
    seg_of = np.repeat(np.arange(nseg), np.diff(offs))
    tick = r0[seg_of] + (np.arange(len(seg_of)) - offs[seg_of])        # row = GameTime - 1
    blocks = {}
    for keep, cols in zip(keeps, qs):
        ki = np.nonzero(keep)[0]
        sg_ = seg_of[ki]
        tk = tick[ki]
        qv = np.stack([((V[ki, c] + q // 2) // q) if q > 1 else V[ki, c] for c, q in cols], 1)
        has1 = np.zeros(len(ki), dtype=bool)
        has1[1:] = sg_[1:] == sg_[:-1]
        has2 = np.zeros(len(ki), dtype=bool)
        has2[2:] = has1[2:] & has1[1:-1]
        prev = np.roll(qv, 1, 0)
        res = qv - prev
        if predictor == 2:
            j = np.nonzero(has2)[0]
            span = np.maximum(tk[j - 1] - tk[j - 2], 1)
            slope = (qv[j - 1] - qv[j - 2]) * ((tk[j] - tk[j - 1]) / span)[:, None]
            res[j] = qv[j] - (qv[j - 1] + np.round(slope).astype(np.int64))
        diff_prev = has1 & np.any(qv != prev, axis=1)
        if fixed_k:
            emit = diff_prev
        else:
            nxt = np.zeros(len(ki), dtype=bool)
            nxt[:-1] = diff_prev[1:]
            emit = has1 & (diff_prev | nxt)
        ei = np.nonzero(emit)[0]
        # ticks since the unit's previous written sample (or its create)
        last_w = np.maximum.accumulate(np.where(emit | ~has1, np.arange(len(ki)), 0))
        gap = tk[ei] - tk[last_w[ei - 1]]
        et, es, dv = tk[ei], slot[sg_[ei]], res[ei]
        order = np.lexsort((es, et))
        et, es, gap, dv = et[order], es[order], gap[order], dv[order]
        blk = et // BLOCK
        cuts = np.searchsorted(blk, np.unique(blk), side="left").tolist() + [len(blk)]
        for bi in range(len(cuts) - 1):
            a, b = cuts[bi], cuts[bi + 1]
            tt, ss = et[a:b], es[a:b]
            ut, cnt = np.unique(tt, return_counts=True)
            sgp = np.diff(np.concatenate(([0], ss)))
            newt = np.concatenate(([True], tt[1:] != tt[:-1]))
            sgp[newt] = ss[newt]
            parts = [varint(np.diff(np.concatenate(([int(blk[a]) * BLOCK], ut)))), varint(cnt),
                     varint(sgp)]
            if not fixed_k:
                parts.append(varint(gap[a:b]))
            for c in range(dv.shape[1]):
                parts.append(varint(dv[a:b, c]))
            blocks.setdefault(int(blk[a]), []).extend(parts)
    raw = deflate = zst = xz = 0
    zc = zstandard.ZstdCompressor(level=19)
    per = {}
    for b, parts in blocks.items():
        data = b"".join(parts)
        raw += len(data)
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        deflate += len(c.compress(data) + c.flush())
        zb = len(zc.compress(data))
        per[b] = zb
        zst += zb
        if WITH_LZMA:
            xz += len(lzma.compress(data, preset=6))
    return {"raw": raw, "deflate2k": deflate, "zstd19": zst, "lzma": xz, "per_block_zstd": per}


# ---------------------------------------------------------------- errors
def err_stats(V, R, moving):
    """Horizontal (x, z) distance, altitude and heading error: percentiles over every live tick
    and over the moving ones (the unit's x or z changed against the tick before or after)."""
    out = {}
    ex = (R[:, 0] - V[:, 0]) / W
    ez = (R[:, 1] - V[:, 1]) / W
    eh = np.hypot(ex, ez)
    ea = np.abs(R[:, 2] - V[:, 2]) / W
    ed = np.abs(R[:, 3] - V[:, 3]) * 360.0 / 65536
    # the pixel the stock draw puts the unit at: the high words of x and z - alt/2 (0x459233..)
    px = (R[:, 0] >> 16) != (V[:, 0] >> 16)
    py = ((R[:, 1] >> 16) - ((R[:, 2] >> 16) >> 1)) != ((V[:, 1] >> 16) - ((V[:, 2] >> 16) >> 1))
    for lab, m in (("all", np.ones(len(V), bool)), ("moving", moving)):
        o = {}
        for nm, e in (("pos_wu", eh), ("alt_wu", ea), ("hd_deg", ed)):
            v = e[m]
            o[nm] = {p: float(np.percentile(v, p)) for p in (50, 90, 99, 99.9)}
            o[nm]["max"] = float(v.max())
            o[nm]["mean"] = float(v.mean())
        o["pixel_differs"] = float((px | py)[m].mean())
        out[lab] = o
    return out


def main():
    d = Path(sys.argv[1])
    V, offs, slot, r0, T = load(d)
    n = len(V)
    seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
    ch = np.zeros(n, dtype=bool)
    dpos = (np.diff(V[:, 0]) != 0) | (np.diff(V[:, 1]) != 0)
    same = seg_of[1:] == seg_of[:-1]
    ch[1:] |= dpos & same
    ch[:-1] |= dpos & same
    print(f"{n:,} live unit-ticks, {len(offs) - 1} lives, moving {ch.sum():,}", flush=True)
    results = []
    forced0 = np.zeros(n, dtype=np.uint8)

    def run(label, keeps_by_col, qcols, mode, fixed_k, extra=None):
        R = np.empty_like(V)
        for c in range(4):
            rebuild(V, offs, keeps_by_col[c], c, qcols[c], mode, R[:, c])
        st = err_stats(V, R, ch)
        groups, seen = [], {}
        for c in range(4):
            key = id(keeps_by_col[c])
            if key not in seen:
                seen[key] = len(groups)
                groups.append((keeps_by_col[c], []))
            groups[seen[key]][1].append((c, qcols[c]))
        sz = {}
        for pred in (1, 2):
            s = stream_size(V, offs, slot, r0, [g[0] for g in groups], [g[1] for g in groups],
                            fixed_k, pred)
            sz[f"pred{pred}"] = s
        nkeep = int(sum(g[0].sum() for g in groups))
        r = {"label": label, "mode": mode, "samples": nkeep, "errors": st, "size": sz}
        if extra:
            r.update(extra)
        results.append(r)
        e = st["moving"]
        print(f"{label:<44} pos p50 {e['pos_wu'][50]:.3f} p99 {e['pos_wu'][99]:.3f} max "
              f"{e['pos_wu']['max']:.2f} | hd p99 {e['hd_deg'][99]:.2f} max {e['hd_deg']['max']:.1f}"
              f" | px {100 * e['pixel_differs']:.1f}% | zstd {sz['pred1']['zstd19'] / 1e6:.2f}/"
              f"{sz['pred2']['zstd19'] / 1e6:.2f} MB defl {sz['pred1']['deflate2k'] / 1e6:.2f}/"
              f"{sz['pred2']['deflate2k'] / 1e6:.2f}", flush=True)

    # fixed rates
    for k in (1, 2, 3, 6, 10):
        keep = np.zeros(n, dtype=np.uint8)
        fixed_samples(offs, r0, k, keep)
        for qp, qh, qlab in ((1, 1, "16.16, u16"), (4096, 1, "1/16 wu, u16"),
                             (W, 256, "whole wu, u8")):
            modes = (1,) if k == 1 else (1, 2)
            for mode in modes:
                run(f"{30 / k:.0f} Hz {qlab} {'lin' if mode == 1 else 'CR'}",
                    [keep] * 4, [qp, qp, qp, qh], mode, True,
                    {"kind": "fixed", "k": k, "qpos": qp, "qhd": qh})
    # error-bounded: position and heading as separate groups, altitude with position
    for eps, epsh, qp, qh in ((0.25, 1.0, 4096, 1), (0.5, 1.0, 4096, 1), (1.0, 2.0, 4096, 1),
                              (1.0, 2.0, W, 256), (2.0, 4.0, 4096, 1), (2.0, 4.0, W, 256)):
        kp = np.zeros(n, dtype=np.uint8)
        tolerance_samples(V, offs, r0, np.array([0, 1]), np.array([qp, qp]), (eps * W) ** 2,
                          0.0, 30, kp, forced0)
        ka = np.zeros(n, dtype=np.uint8)
        tolerance_samples(V, offs, r0, np.array([2]), np.array([qp]), 0.0, eps * W, 30, ka,
                          forced0)
        kh = np.zeros(n, dtype=np.uint8)
        tolerance_samples(V, offs, r0, np.array([3]), np.array([qh]), 0.0, epsh * 65536 / 360,
                          30, kh, forced0)
        run(f"tol {eps} wu {epsh} deg q {qp // 4096}/16 wu, hd {65536 // qh} steps",
            [kp, kp, ka, kh], [qp, qp, qp, qh], 1, False,
            {"kind": "tolerance", "eps": eps, "eps_hd": epsh, "qpos": qp, "qhd": qh})
    Path(sys.argv[2]).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
