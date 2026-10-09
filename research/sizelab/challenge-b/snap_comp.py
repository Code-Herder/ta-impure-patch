"""Each snapshot block's payload under the writer's deflate (2 KB window), zlib 9 (32 KB window,
the class libdeflate and miniz belong to), zstd 3 and zstd 19."""
import sys, zlib, zstandard
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
bl, stop = T.blocks(sys.argv[1])
def w(b): c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
z3 = zstandard.ZstdCompressor(level=3); z19 = zstandard.ZstdCompressor(level=19)
tot = [0, 0, 0, 0, 0]
for kind, first, last, p in bl:
    if kind != T.RB_SNAPSHOT: continue
    r = [len(p), w(p), len(zlib.compress(p, 9)), len(z3.compress(p)), len(z19.compress(p))]
    tot = [a + b for a, b in zip(tot, r)]
    print(last, *r)
print("total raw/writer/zlib9/zstd3/zstd19 KiB:", *[round(t / 1024) for t in tot])
