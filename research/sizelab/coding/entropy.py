"""Entropy coding beyond LZ, on the column streams of the delta kinds' values.

For every block (N iterations, a block also ends at each snapshot), the changed values of each
(kind, dword) column, as the change against the key's previous value (T1) or against a linear
extrapolation from its last two values (T3, order 2; reset at each snapshot), are coded:

  zstd       zstd 3 / 19 on the columns concatenated (the L2 layout), alone per block
  colzstd    zstd 19 on each column alone (Parquet-style)
  ans0       an adaptive order-0 model per column of the value's bucket (zigzag bit length,
             0..33), the bits below the top bit raw -- what a per-column rANS with adaptive
             frequencies codes to within a fraction of a percent [the cost is -log2 of the
             adaptive (KT) probability; MEASURED as a model cost, not a coder's output]
  ans1u      the bucket's model conditioned on the same key's previous bucket for that dword
  ans1n      ... conditioned on the previous value's bucket in the column (the neighbour)
  bytes0     an adaptive order-0 model of each byte plane of the column (no transform)

Models reset at every block, so each block decodes given the world at its start.

    entropy.py battle N
"""
import sys, struct, collections, time, json
import numpy as np
import zstandard
import reencode as R
from common import *
T = R.T

name = sys.argv[1]; N = int(sys.argv[2])
bl = load(name)
for k, f, l, p, pk in bl:
    if k == RB_STREAM:
        for kind, b in records(p):
            if kind == T.RK_GLOBALS: R.GLOB_DW = len(b) // 4
        break
snaps = {l for k, f, l, p, pk in bl if k == RB_SNAPSHOT}
blocks = []; cur = []
for k, f, l, p, pk in bl:
    if k != RB_STREAM: continue
    if N == 0:
        blocks.append(p); continue
    for t, b in iterations(p):
        cur.append(b)
        if len(cur) >= N or t in snaps:
            blocks.append(b"".join(cur)); cur = []
if cur: blocks.append(b"".join(cur))

def kt_cost(sym, ctx=None, A=34):
    """Adaptive KT-estimator cost in bits of the sequence sym (ints < A), optionally per context."""
    sym = np.asarray(sym, dtype=np.int64)
    n = len(sym)
    if n == 0: return 0.0
    if ctx is None:
        key = sym; ckey = np.zeros(n, dtype=np.int64)
    else:
        ctx = np.asarray(ctx, dtype=np.int64)
        key = ctx * A + sym; ckey = ctx
    def occ(k):
        order = np.argsort(k, kind="stable")
        ks = k[order]
        start = np.r_[0, np.flatnonzero(np.diff(ks)) + 1]
        rank = np.arange(n) - np.repeat(start, np.diff(np.r_[start, n]))
        o = np.empty(n, dtype=np.int64); o[order] = rank
        return o
    c_sym = occ(key); c_ctx = occ(ckey)
    return float(-np.sum(np.log2((c_sym + 0.5) / (c_ctx + 0.5 * A))))

def zigzag(d):
    d = d.astype(np.int64)
    d = np.where(d >= 2**31, d - 2**32, d)
    return np.where(d >= 0, 2 * d, -2 * d - 1)

def bucket(z):
    z = z.astype(np.float64)
    return np.where(z == 0, 0, np.floor(np.log2(np.maximum(z, 1))).astype(np.int64) + 1).astype(np.int64)

z3 = zstandard.ZstdCompressor(level=3); z19 = zstandard.ZstdCompressor(level=19)
tot = collections.Counter()
prev = {}        # (kind, key) -> [last, last2] lists of 2 arrays
lastb = {}       # (kind, key, dword) -> last bucket (for ans1u), reset per block
t0 = time.time()
for bi, p in enumerate(blocks):
    if bi and p[:4] and False: pass
    # parse the delta kinds' values with their keys
    rows = collections.defaultdict(list)   # col -> list of (key, v, d1, d2)
    for kind, b in records(p):
        if kind == T.RK_TICK:
            tick = struct.unpack_from("<I", b)[0]
            if tick - 1 in snaps or tick in snaps:
                pass
        if kind in R.IMAGE_OF:
            dk = R.IMAGE_OF[kind]
            img = b if kind == T.RK_GLOBALS else b[4:]
            key = 0 if kind == T.RK_GLOBALS else struct.unpack_from("<H", b)[0]
            vs = list(struct.unpack_from(f"<{len(img) // 4}I", img))
            prev[(dk, key)] = [vs, list(vs)]
            continue
        ds = R.delta_spec(kind)
        if not ds: continue
        kb, nb, hd, ndw = ds
        key = struct.unpack_from("<H", b)[0] if kb else 0
        st = prev.get((kind, key))
        if st is None:
            st = prev[(kind, key)] = [[0] * ndw, [0] * ndw]
        last, last2 = st
        for k2, d in enumerate(R.bits_list(b[kb:kb + nb])):
            v = struct.unpack_from("<I", b, hd + 4 * k2)[0]
            d1 = (v - last[d]) & 0xFFFFFFFF
            pred = (2 * last[d] - last2[d]) & 0xFFFFFFFF
            d2 = (v - pred) & 0xFFFFFFFF
            rows[kind * 256 + d].append((key, v, d1, d2))
            last2[d] = last[d]; last[d] = v
    # at a snapshot the order-2 history resets: last2 = last (a seek has one value)
    # (blocks end at snapshots, so the reset happens between blocks)
    if True:
        # find whether this block ended at a snapshot tick
        its = iterations(p)
        if its and its[-1][0] in snaps:
            for st in prev.values(): st[1] = list(st[0])
    # coders
    lay = {"T0": bytearray(), "T1": bytearray(), "T3": bytearray()}
    ans = collections.Counter()
    lastb = {}
    for col in sorted(rows):
        r = rows[col]
        keys = np.array([x[0] for x in r], dtype=np.int64)
        v = np.array([x[1] for x in r], dtype=np.uint32)
        d1 = np.array([x[2] for x in r], dtype=np.uint32)
        d2 = np.array([x[3] for x in r], dtype=np.uint32)
        lay["T0"] += v.tobytes(); lay["T1"] += d1.tobytes(); lay["T3"] += d2.tobytes()
        for tn, arr in (("T1", d1), ("T3", d2)):
            z = zigzag(arr); bk = bucket(z)
            raw_bits = float(np.sum(np.maximum(bk - 1, 0)))
            ans[f"ans0 {tn}"] += kt_cost(bk) + raw_bits
            ans[f"ans1n {tn}"] += kt_cost(bk, np.r_[34, bk[:-1]], 34) + raw_bits if len(bk) else 0
            # same key's previous bucket for this column, within the block
            ctx = np.empty(len(bk), dtype=np.int64)
            lb = {}
            for i, kk in enumerate(keys):
                ctx[i] = lb.get(kk, 34); lb[kk] = bk[i]
            ans[f"ans1u {tn}"] += kt_cost(bk, ctx, 34) + raw_bits
            ans[f"colzstd19 {tn}"] += 8 * len(z19.compress(arr.tobytes()))
        planes = v.view(np.uint8).reshape(-1, 4)
        ans["bytes0 T0"] += sum(kt_cost(planes[:, j], None, 256) for j in range(4))
        planes = d1.view(np.uint8).reshape(-1, 4)
        ans["bytes0 T1"] += sum(kt_cost(planes[:, j], None, 256) for j in range(4))
    for k_, v_ in ans.items(): tot[k_] += v_ / 8
    for tn, blob in lay.items():
        tot[f"zstd3 {tn}"] += len(z3.compress(bytes(blob)))
        tot[f"zstd19 {tn}"] += len(z19.compress(bytes(blob)))
        tot[f"raw {tn}"] += len(blob)
    if bi % 100 == 0:
        print(f"block {bi}/{len(blocks)} {time.time()-t0:.0f}s", flush=True)
print(f"\n{name} N={N}: the delta kinds' changed values only (unit, projectile, player, globals), MiB")
for k_ in sorted(tot):
    print(f"  {k_:<22}{tot[k_]/2**20:9.2f}")
json.dump(dict(tot), open(f"entropy-{name}-N{N}.json", "w"), indent=1)
