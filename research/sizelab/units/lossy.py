#!/usr/bin/env python3
"""lossy.py -- the unit stream with the motion lanes sampled and interpolated, everything else exact.

    lossy.py mode=hz10            every third iteration, whole world units, 8-bit turns
    lossy.py mode=eps pos=1 deg=2 [q=16]   a sample only when interpolation would miss by more than
                                  pos world units or deg degrees (q: position steps per wu kept)

MOTION = position x/y/z (+0x6A/+0x6E/+0x72), the turns (+0x64/+0x66/+0x68), the mover's velocity
(+0x08..+0x10), speed (+0x20), turn rate (+0x24) and moved-stamp (+0x2A). They leave the exact
frame; every other lane stays in frame2's codec, exact at every iteration (checked by decoding).
Anchors that cost nothing: a unit's first iteration (RK_UNIT carries its image), its last (the
death event carries its image), and each snapshot. Samples are coded as residuals against a linear
extrapolation of the unit's two previous samples (in quantized units), units with all-zero
residuals omitted when the sample times are implicit (hz10). The decoder interpolates linearly
between consecutive samples/anchors (headings on the short arc); the error is measured against
the recording's exact values at every unit-iteration.
"""
import json
import sys
import time

import numpy as np

import frame2
import sim
from frame import varint, zz
from run import comp_all

opts = dict(a.split("=", 1) for a in sys.argv[1:])
MODE = opts.get("mode", "hz10")
EPS_POS = float(opts.get("pos", 1))
EPS_DEG = float(opts.get("deg", 2))
QPOS = int(opts.get("q", 1))          # position quantum: 1/QPOS world units
QH = 256                              # heading quantum: 65536/256 -> 8 bits

L = lambda k: np.asarray(np.load(f"series/{k}.npy", mmap_mode="r"))
live = L("live")
NT, NS = live.shape
gt = np.load("series/gametime.npy").astype(np.int64)
z = sim.load("units.npz")
snaps = set(int(t) for t in z["snap_ticks"])
snap_rows = np.array([i for i in range(NT) if gt[i] in snaps])
snap_set = set(int(r) for r in snap_rows)
BLOCKCUT = opts.get("blockcut") == "1"
_tb = z["ops"][z["ops"][:, 0] == sim.OP_TICK][:, 1]
block_last = [i for i in range(NT) if i == NT - 1 or _tb[i] != _tb[i + 1]]

# runs: (slot, first row, last row), a new run at each RK_UNIT
ops = z["ops"]
row = np.cumsum(ops[:, 0] == sim.OP_TICK) - 1
created = np.zeros((NT, NS), bool)
u = ops[:, 0] == sim.OP_UNIT
created[row[u], ops[u, 2]] = True
runs = []
for s in range(NS):
    lv = live[:, s]
    if not lv.any():
        continue
    starts = np.flatnonzero(lv & (created[:, s] | ~np.concatenate(([False], lv[:-1]))))
    for a in starts:
        b = a
        ends = np.flatnonzero(~lv[a:])
        b = a + (ends[0] if len(ends) else len(lv) - a) - 1
        runs.append((s, a, b))

# exact values (float, world units / turn units)
X = {k: L(k).astype(np.float64) for k in ("x", "y", "z")}
H = {k: L(k).astype(np.float64) for k in ("heading", "pitch", "roll")}
for k in X:
    X[k] /= 65536.0


def qpos(v):
    return np.round(v * QPOS) / QPOS


def qturn(h):
    return (np.round(h / QH) * QH) % 65536


def lerp_turn(a, b, f):
    d = (b - a + 32768) % 65536 - 32768
    return (a + d * f) % 65536


def turn_err(a, b):
    d = np.abs((a - b + 32768) % 65536 - 32768)
    return d * 360.0 / 65536


def interp(keep, vals, n, turn):
    """keep: sorted sample indices into 0..n-1 (incl. 0 and n-1), vals at those; -> n values."""
    out = np.empty(n)
    k = np.searchsorted(keep, np.arange(n), side="right") - 1
    k = np.clip(k, 0, len(keep) - 2) if len(keep) > 1 else np.zeros(n, int)
    if len(keep) == 1:
        return np.full(n, vals[0])
    a, b = keep[k], keep[k + 1]
    f = (np.arange(n) - a) / np.maximum(b - a, 1)
    if turn:
        return lerp_turn(vals[k], vals[k + 1], f)
    return vals[k] + (vals[k + 1] - vals[k]) * f


def greedy(P, Hh, anchors, n, forced=frozenset()):
    """Error-bounded keep set: extend each segment while the interpolation of the QUANTIZED end
    points stays within the bounds at every skipped iteration."""
    keep = [0]
    a = 0
    anchors = set(anchors)
    qP = [qpos(p) for p in P]
    qH = [qturn(h) for h in Hh]
    for i in anchors:                      # anchors decode exact, so check against exact ends
        for p, qp in zip(P, qP):
            qp[i] = p[i]
        for h, qh in zip(Hh, qH):
            qh[i] = h[i]
    e = a + 1
    while e < n:
        # try extending to e+1 unless e is an anchor
        if e in anchors or e in forced or e == n - 1:
            keep.append(e)
            a, e = e, e + 1
            continue
        c = e + 1
        idx = np.arange(a + 1, c)
        f = (idx - a) / (c - a)
        ok = True
        for p, qp in zip(P, qP):
            lp = qp[a] + (qp[c] - qp[a]) * f
            if np.abs(lp - p[idx]).max() > EPS_POS:
                ok = False
                break
        if ok:
            for h, qh in zip(Hh, qH):
                lh = lerp_turn(qh[a], qh[c], f)
                if turn_err(lh, h[idx]).max() > EPS_DEG:
                    ok = False
                    break
        if ok:
            e = c
        else:
            keep.append(e)
            a, e = e, e + 1
    return np.array(sorted(set(keep)))


t0 = time.time()
samples = []          # (row, slot, qx, qy, qz, qh, qp, qr) for kept non-anchor samples
errs_p, errs_h = [], []
n_keep = 0
for (s, a, b) in runs:
    n = b - a + 1
    rows = np.arange(a, b + 1)
    P = [X[k][a:b + 1, s] for k in ("x", "y", "z")]
    Hh = [H[k][a:b + 1, s] for k in ("heading", "pitch", "roll")]
    anchors = {0, n - 1} | {r - a for r in snap_rows if a <= r <= b}
    if MODE == "hz10":
        keep = np.array(sorted(anchors | {i for i in range(n) if gt[a + i] % 3 == 0}))
    else:
        forced = {r - a for r in block_last if a <= r <= b} if BLOCKCUT else set()
        keep = greedy(P, Hh, anchors, n, forced)
    # anchors are exact; samples quantized
    isanc = np.isin(keep, list(anchors))
    vp = []
    for p in P:
        v = qpos(p[keep]); v[isanc] = p[keep][isanc]; vp.append(v)
    vh = []
    for h in Hh:
        v = qturn(h[keep]); v[isanc] = h[keep][isanc]; vh.append(v)
    for p, v in zip(P, vp):
        errs_p.append(np.abs(interp(keep, v, n, False) - p))
    for h, v in zip(Hh[:1], vh[:1]):
        errs_h.append(turn_err(interp(keep, v, n, True), h))
    n_keep += int((~isanc).sum())
    for i, anc in zip(keep, isanc):
        kind = 0 if not anc else (2 if (i == 0 or (a + i) in snap_set) else 1)
        samples.append((a + i, s, kind, *(int(round(p[i] * QPOS)) for p in P),
                        *(int(round(h[i] / QH)) % 256 for h in Hh)))
t1 = time.time()
ep = np.concatenate(errs_p)
eh = np.concatenate(errs_h)
samples.sort()
S = np.array(samples, np.int64).reshape(-1, 9)

# ---- the exact part: frame2 without the motion lanes ----------------------------------------
enc = frame2.Codec(lanes="field", rules=True, pred="win", reset_ticks=snaps)
ln = enc.ln
M_ = 0x118
motion = [ln.index[o] for o in (0x6A, 0x6E, 0x72, 0x64, 0x66, 0x68, M_ + 8, M_ + 0xC, M_ + 0x10,
                                M_ + 0x20, M_ + 0x24, M_ + 0x2A)]


def motion_bytes(rows_in_block, state):
    """The block's samples: per iteration the sampled slots (gaps), then per field the residual
    against the unit's linear extrapolation (in quantized units) from its two previous samples or
    anchors. Kind 2 anchors (creation, snapshot) reset the history -- a reader can start there --
    and kind 1 (death) ends it; neither is coded. hz10: units whose residuals are all zero are
    omitted (the sample times are implicit); eps: every kept sample is listed."""
    lo, hi = rows_in_block[0], rows_in_block[-1]
    sel = S[(S[:, 0] >= lo) & (S[:, 0] <= hi)]
    per_tick = {r: [] for r in rows_in_block}
    for rec in sel:
        r, s, kind = int(rec[0]), int(rec[1]), int(rec[2])
        q = rec[3:].copy()
        if kind == 2:
            state[s] = [(r, q)]
            continue
        hist = state.get(s, [])
        if len(hist) >= 2:
            (t2, q2), (t1_, q1) = hist[-2], hist[-1]
            d = q1 - q2
            d[3:] = (d[3:] + 128) % 256 - 128
            pred = q1 + np.round(d * (r - t1_) / max(t1_ - t2, 1)).astype(np.int64)
        elif hist:
            pred = hist[-1][1]
        else:
            pred = np.zeros_like(q)
        res = q - pred
        res[3:] = (res[3:] + 128) % 256 - 128
        state[s] = (hist + [(r, q)])[-2:]
        if kind == 1:
            continue
        if MODE == "eps" or (res != 0).any():
            per_tick[r].append((s, res))
    out = [varint([len(v) for v in per_tick.values()])]
    sl, rs = [], []
    for r, v in per_tick.items():
        if v:
            ss = np.array([a for a, _ in v])
            sl.append(np.diff(np.concatenate(([-1], ss))) - 1)
            rs.append(np.array([b for _, b in v]))
    if sl:
        out.append(varint(np.concatenate(sl)))
        R = np.concatenate(rs)
        for j in range(R.shape[1]):
            out.append(varint(zz(R[:, j])))
    return b"".join(out)


sections = []
sections_exact = []
mstate = {}
group, gkey, rowi = [], None, 0
rows_of_block = []


def reset_state_for(tk):
    # a unit created this iteration starts from its exact image: forget its sample history
    for c in tk.created:
        mstate[c] = (None, (None, None))


def flush(group, rows):
    if not group:
        return
    fr = []
    for tk in group:
        s, R = enc.encode_tick(tk)
        R[:, motion] = 0
        keep = (R != 0).any(axis=1)
        fr.append((s[keep], R[keep]))
    exact = enc.block_bytes(fr)
    sections_exact.append(exact)
    sections.append(exact + motion_bytes(rows, mstate))


i = 0
for tk in sim.ticks(z):
    if tk.block != gkey:
        flush(group, rows_of_block)
        group, gkey, rows_of_block = [], tk.block, []
    group.append(tk._replace(S=tk.S.copy(), live=tk.live.copy()))
    rows_of_block.append(i)
    i += 1
flush(group, rows_of_block)
raw = sum(len(s) for s in sections)
res = comp_all(sections)
res_exact = comp_all(sections_exact)
tag = f"lossy-{MODE}" + (f"-pos{EPS_POS}-deg{EPS_DEG}-q{QPOS}" if MODE == "eps" else f"-q{QPOS}") + ("-blockcut" if BLOCKCUT else "")
out = {"tag": tag, "raw": raw, "samples": int(n_keep), "unit_ticks": int(len(ep) // 3),
       "pos_err_wu": {"mean": round(float(ep.mean()), 4), "p99": round(float(np.percentile(ep, 99)), 3),
                      "max": round(float(ep.max()), 3)},
       "heading_err_deg": {"mean": round(float(eh.mean()), 4), "p99": round(float(np.percentile(eh, 99)), 3),
                           "max": round(float(eh.max()), 3)},
       "sample_s": round(t1 - t0, 1), **res,
       "exact_part": {k: v for k, v in res_exact.items()}}
print(json.dumps(out))
with open("results.jsonl", "a") as f:
    f.write(json.dumps(out) + "\n")
if opts.get("save") == "1":
    import pickle
    with open(f"out-{tag}.pkl", "wb") as f:
        pickle.dump(sections, f)
