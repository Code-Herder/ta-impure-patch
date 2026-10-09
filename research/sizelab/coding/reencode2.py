"""reencode.py's framing/value variants, regrouped into blocks of N iterations (0 = today's blocks),
compressed alone and chained (one zstd frame per snapshot interval, flushed at each block).

    reencode2.py battle H,D,T,L N [--dump]
"""
import sys, time, json, lzma, zlib, pickle
import zstandard, brotli
import reencode as R
from common import *

name = sys.argv[1]
H, D, Tv, L = (int(c) for c in sys.argv[2].split(","))
N = int(sys.argv[3])
bl = load(name)
for k, f, l, p, pk in bl:
    if k == RB_STREAM:
        for kind, b in records(p):
            if kind == R.T.RK_GLOBALS: R.GLOB_DW = len(b) // 4
        break
# the blocks: (chain, payload)
blocks = []
chain = 0
if N == 0:
    for k, f, l, p, pk in bl:
        if k == RB_SNAPSHOT: chain += 1
        if k == RB_STREAM: blocks.append((chain, p))
else:
    snaps = {l for k, f, l, p, pk in bl if k == RB_SNAPSHOT}
    cur = []
    for k, f, l, p, pk in bl:
        if k != RB_STREAM: continue
        for t, b in iterations(p):
            cur.append(b)
            if len(cur) >= N or t in snaps:
                blocks.append((chain, b"".join(cur))); cur = []
                if t in snaps: chain += 1
    if cur: blocks.append((chain, b"".join(cur)))
we = R.World(); wd = R.World()
PERSIST = '--persist' in sys.argv
lme = {}; lmd = {}; last_ch = None
blobs = []
t0 = time.time()
ok = True
for ch, p in blocks:
    if not PERSIST or ch != last_ch: lme = {}; lmd = {}
    last_ch = ch
    fr, vals = R.encode_block(p, H, D, Tv, we, lme)
    blob = R.layout(fr, vals, L)
    blobs.append((ch, blob))
    if "--nocheck" not in sys.argv and ok:
        values = [t for _, t, _ in vals] if L == 0 else R.unlayout(blob, len(fr), [c for c, _, _ in vals], L)
        if R.decode_block(fr if L == 0 else blob[:len(fr)], values, H, D, Tv, wd, lmd) != p:
            ok = False; print("ROUND TRIP FAILED")
print(f"H{H}D{D}T{Tv}L{L} N{N}{' persist' if PERSIST else ''}: {len(blobs)} blocks, raw {sum(len(b) for _, b in blobs)/2**20:.1f} MiB, encoded in {time.time()-t0:.0f}s, roundtrip {'OK' if ok else 'FAIL'}", flush=True)
if "--dump" in sys.argv:
    pickle.dump(blobs, open(f"blobs-{name}-H{H}D{D}T{Tv}L{L}-N{N}.pkl", "wb"))
res = {}
def alone(label, f):
    t = time.perf_counter(); tot = sum(f(b) for _, b in blobs); dt = time.perf_counter() - t
    res[label] = tot; print(f"  {label:<26}{tot/2**20:8.2f} MiB  {sum(len(b) for _, b in blobs)/2**20/dt:7.1f} MB/s", flush=True)
def chained(label, level, wlog=0, ldm=False):
    t = time.perf_counter()
    p = zstandard.ZstdCompressionParameters.from_level(level, window_log=wlog, enable_ldm=ldm, write_content_size=0) if (wlog or ldm) else None
    c = zstandard.ZstdCompressor(level=level, write_content_size=False) if p is None else zstandard.ZstdCompressor(compression_params=p)
    tot = 0; cur = None; obj = None
    for ch, b in blobs:
        if ch != cur:
            if obj is not None: tot += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
            obj = c.compressobj(); cur = ch
        tot += len(obj.compress(b)) + len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK))
    tot += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
    dt = time.perf_counter() - t
    res[label] = tot; print(f"  {label:<26}{tot/2**20:8.2f} MiB  {sum(len(b) for _, b in blobs)/2**20/dt:7.1f} MB/s", flush=True)
def defl(b):
    c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
alone("deflate2K", defl)
for lv in ((1, 3, 9, 19) if "--skipbase" not in sys.argv else ()):
    c = zstandard.ZstdCompressor(level=lv)
    alone(f"zstd{lv} alone", lambda b, c=c: len(c.compress(b)))
for lv in ((1, 3, 5, 9, 19) if "--skipbase" not in sys.argv else ()):
    chained(f"zstd{lv} chain", lv)
if "--more" in sys.argv:
    chained("zstd3 chain wlog24", 3, 24)
    chained("zstd3 chain wlog24 ldm", 3, 24, True)
    chained("zstd9 chain wlog24", 9, 24)
    alone("brotli q5 w22", lambda b: len(brotli.compress(b, quality=5, lgwin=22)))
    alone("lzma 6", lambda b: len(lzma.compress(b, preset=6)))
    filt = [{"id": lzma.FILTER_LZMA2, "preset": 6, "lc": 0, "lp": 2, "pb": 2}]
    alone("lzma 6 lp2pb2", lambda b: len(lzma.compress(b, format=lzma.FORMAT_RAW, filters=filt)))
json.dump(res, open(f"reencode2-{name}-H{H}D{D}T{Tv}L{L}-N{N}{'p' if PERSIST else ''}.json", "w"), indent=1)
