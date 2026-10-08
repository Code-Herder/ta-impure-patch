"""Independent re-derivation: today's stream blocks, each payload compressed alone by zstd 3,
against the packed bytes on disk. Also iterations per block."""
import sys, zstandard, numpy as np, struct, zlib
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
p = sys.argv[1]
sizes = []
bl, stop = T.blocks(p, sizes)
assert stop is None
c3 = zstandard.ZstdCompressor(level=3)
tot = {"packed": 0, "raw": 0, "z3": 0, "w2k": 0}
iters = []
for (kind, first, last, payload), (_, _, _, raw, packed) in zip(bl, sizes):
    if kind != T.RB_STREAM: continue
    tot["packed"] += packed; tot["raw"] += raw
    tot["z3"] += len(c3.compress(payload))
    c = zlib.compressobj(6, zlib.DEFLATED, 11); tot["w2k"] += len(c.compress(payload) + c.flush())
    iters.append(last - first + 1)
print({k: round(v / 2**20, 3) for k, v in tot.items()})
it = np.array(iters)
print("stream blocks", len(it), "iterations/block mean", it.mean().round(2), "median", np.median(it), "min", it.min(), "max", it.max())
print("sum of iterations", it.sum())
