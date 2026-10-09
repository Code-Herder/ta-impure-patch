"""L0: zstd level 3 as a drop-in per block on a whole recording (coding/'s comp_sweep covers the
stream only). Every block of the file, each compressed alone; the whole file = magic + 32-byte
block headers + packed bodies. Decoded back and compared. Times are 64-bit Python bindings,
best of 3 passes over all blocks.  usage: zstd_dropin.py <file.imprec>"""
import collections, sys, time, zlib
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
import zstandard

path = sys.argv[1]
sizes = []
bl, stop = T.blocks(path, sizes)
assert stop is None, stop
fsize = __import__("os").path.getsize(path)
names = {T.RB_HEADER: "header", T.RB_STREAM: "stream", T.RB_SNAPSHOT: "snapshot", T.RB_END: "end",
         getattr(T, "RB_MINIMAP", 5): "minimap"}
zc = zstandard.ZstdCompressor(level=3)
zd = zstandard.ZstdDecompressor()
payloads = [p for _, _, _, p in bl]
raw = collections.Counter(); wr = collections.Counter(); z3 = collections.Counter(); n = collections.Counter()
outs = []
for (kind, f, l, p), (_, _, _, r, pk) in zip(bl, sizes):
    o = zc.compress(p)
    outs.append(o)
    k = names.get(kind, str(kind))
    n[k] += 1; raw[k] += r; wr[k] += pk; z3[k] += len(o)
for o, p in zip(outs, payloads):
    assert zd.decompress(o) == p
MiB = 1048576
print(f"{path.split('/')[-1]}: {len(bl)} blocks, file {fsize / MiB:.2f} MiB")
print(f"{'block kind':<10}{'n':>6}{'raw MiB':>10}{'writer MiB':>12}{'zstd3 MiB':>11}{'ratio':>7}")
for k in raw:
    print(f"{k:<10}{n[k]:>6}{raw[k] / MiB:>10.2f}{wr[k] / MiB:>12.2f}{z3[k] / MiB:>11.2f}{z3[k] / wr[k]:>7.3f}")
over = 8 + 32 * len(bl)
W, Z = sum(wr.values()) + over, sum(z3.values()) + over
print(f"{'FILE':<10}{len(bl):>6}{sum(raw.values()) / MiB:>10.2f}{W / MiB:>12.2f}{Z / MiB:>11.2f}{Z / W:>7.3f}"
      f"   (check: writer sum + framing {W} vs file {fsize})")
# the writer's blocks as stored, inflated; zstd-3 decode; zstd-3 encode
stored = []
with open(path, "rb") as fh:
    data = fh.read()
at = 8
for _ in bl:
    import struct
    magic, kind, flags, first, last, r, pk, cb, ch = struct.unpack_from("<IHHIIIIII", data, at)
    stored.append((flags, data[at + 32:at + 32 + pk]))
    at += 32 + pk
def best(fn):
    t = []
    for _ in range(3):
        s = time.perf_counter(); fn(); t.append(time.perf_counter() - s)
    return min(t)
tz = best(lambda: [zlib.decompress(b) if not (fl & T.RBF_STORED) else b for fl, b in stored])
td = best(lambda: [zd.decompress(o) for o in outs])
tc = best(lambda: [zc.compress(p) for p in payloads])
R = sum(raw.values()) / 1e6
print(f"decode whole file: writer (zlib inflate) {tz * 1000:.0f} ms ({R / tz:.0f} MB/s raw), "
      f"zstd3 {td * 1000:.0f} ms ({R / td:.0f} MB/s)  [64-bit Python]")
print(f"encode zstd3: {tc * 1000:.0f} ms ({R / tc:.0f} MB/s)")
# the chained form coding/ measured (one zstd-3 frame per snapshot interval, flushed at each
# stream block, ended before each snapshot), for the block-length question; decoded back
tot = 0
co = None
chunks = []
for (kind, f, l, p) in bl:
    if kind == T.RB_STREAM:
        if co is None:
            co = zc.compressobj(); chunks.append([])
        o = co.compress(p) + co.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK)
        chunks[-1].append((o, p))
        tot += len(o)
    else:
        if co is not None:
            e = co.flush(); tot += len(e); chunks[-1].append((e, b"")); co = None
        tot += len(zc.compress(p))
if co is not None:
    e = co.flush(); tot += len(e); chunks[-1].append((e, b""))
for ch in chunks:
    do = zd.decompressobj()
    got = b"".join(do.decompress(o) for o, _ in ch)
    assert got == b"".join(p for _, p in ch)
print(f"chained zstd3 (one frame per snapshot interval, flush per block): file {(tot + over) / MiB:.2f} MiB "
      f"({(tot + over) / W:.3f} of the writer)  [decoded: equal]")
print(f"iterations per stream block: mean {sum(l - f + 1 for k, f, l, _ in bl if k == T.RB_STREAM) / n['stream']:.1f}")
