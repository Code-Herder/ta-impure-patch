"""Framing by category: raw bytes of headers, padding, slots, masks, reserved zeros and other heads,
and each category's packed size alone (deflate 2 KB, zstd 3, zstd 9 chained) under today's
framing and two re-encodings, as recsize.py attributes: each block's records of one category
together, with the block's TICK records (whose own cost is subtracted).

    catattr.py battle
"""
import sys, struct, collections, zlib, json
import zstandard
import reencode as R
from common import *
T = R.T
GROUP = {T.RK_UNIT_DELTA: "unit deltas", T.RK_PROJ_DELTA: "projectiles", T.RK_PROJ: "projectiles",
         T.RK_PROJ_HEAD: "projectiles", T.RK_CELLS: "cells", T.RK_ORDERS: "orders",
         T.RK_UNIT: "unit images+gone", T.RK_UNIT_GONE: "unit images+gone"}
for k in T.EVENTS: GROUP[k] = "events"
def group(k):
    if k == T.RK_TICK: return None
    return GROUP.get(k, "other")

name = sys.argv[1] if len(sys.argv) > 1 else "battle"
bl = load(name)
for k, f, l, p, pk in bl:
    if k == RB_STREAM:
        for kind, b in records(p):
            if kind == T.RK_GLOBALS: R.GLOB_DW = len(b) // 4
        break

# raw framing census
raw = collections.defaultdict(collections.Counter)
for k, f, l, p, pk in bl:
    if k != RB_STREAM: continue
    for kind, b in records(p):
        g = group(kind) or "iteration marks"
        c = raw[g]
        c["records"] += 1
        c["header"] += 4
        c["pad"] += -len(b) % 4
        ds = R.delta_spec(kind)
        if ds:
            kb, nb, hd, ndw = ds
            c["slot"] += kb; c["mask"] += nb; c["reserved"] += hd - kb - nb; c["values"] += len(b) - hd
        elif kind in (T.RK_CELLS, T.RK_ORDERS):
            c["head"] += 8; c["values"] += len(b) - 8
        elif kind in (T.RK_UNIT, T.RK_PROJ):
            c["slot"] += 2; c["reserved"] += 2; c["values"] += len(b) - 4
        else:
            c["values"] += len(b)
cols = ["records", "header", "pad", "slot", "mask", "reserved", "head", "values"]
print(f"{'category':<20}" + "".join(f"{c:>10}" for c in cols) + "   (MiB, records in thousands)")
for g, c in sorted(raw.items(), key=lambda kv: -sum(kv[1].values())):
    print(f"{g:<20}" + f"{c['records']/1e3:>10.0f}" + "".join(f"{c[x]/2**20:>10.2f}" for x in cols[1:]))

# packed alone, per variant
def defl(b):
    c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
z3 = zstandard.ZstdCompressor(level=3)
VARIANTS = {"today": (0, 0, 0, 1), "H3D1": (3, 1, 0, 1), "H3D4T1L2": (3, 4, 1, 2)}
out = {"raw": {g: dict(c) for g, c in raw.items()}, "packed": {}}
for vn, (H, D, Tv, L) in VARIANTS.items():
    worlds = collections.defaultdict(R.World)
    tot = collections.Counter()
    chains = {}
    chain = 0
    for k, f, l, p, pk in bl:
        if k == RB_SNAPSHOT:
            chain += 1
            for g, obj in chains.items(): tot[(g, "zstd9 chain")] += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
            chains = {}
        if k != RB_STREAM: continue
        parts = collections.defaultdict(bytearray)
        at = 0
        while at + 4 <= len(p):
            kind, ln = struct.unpack_from("<HH", p, at)
            step = (4 + ln + 3) & ~3
            g = group(kind)
            if g is None:
                for gg in ("ticks", "unit deltas", "projectiles", "cells", "orders", "events", "unit images+gone", "other"):
                    parts[gg] += p[at:at + step]
            else:
                parts[g] += p[at:at + step]
            at += step
        for g, sub in parts.items():
            fr, vals = R.encode_block(bytes(sub), H, D, Tv, worlds[g])
            blob = R.layout(fr, vals, L)
            tot[(g, "deflate2K")] += defl(blob)
            tot[(g, "zstd3")] += len(z3.compress(blob))
            if g not in chains:
                chains[g] = zstandard.ZstdCompressor(level=9, write_content_size=False).compressobj()
            tot[(g, "zstd9 chain")] += len(chains[g].compress(blob)) + len(chains[g].flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK))
    for g, obj in chains.items(): tot[(g, "zstd9 chain")] += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
    res = {}
    for g in ("unit deltas", "projectiles", "cells", "orders", "events", "unit images+gone", "other"):
        res[g] = {c: tot[(g, c)] - tot[("ticks", c)] for c in ("deflate2K", "zstd3", "zstd9 chain")}
    res["ticks"] = {c: tot[("ticks", c)] for c in ("deflate2K", "zstd3", "zstd9 chain")}
    out["packed"][vn] = res
    print(f"\n{vn}: packed alone (MiB), each less the ticks-only stream")
    for g, r in res.items():
        print(f"  {g:<20}" + "".join(f"{c} {v/2**20:8.2f}  " for c, v in r.items()), flush=True)
json.dump(out, open(f"catattr-{name}.json", "w"), indent=1)
