"""Drop-in compressors on today's stream blocks: ratio and (64-bit, Python binding) speed."""
import sys, time, zlib, lzma
import zstandard, brotli, lz4.frame, lz4.block
from common import *
bl = load(sys.argv[1] if len(sys.argv) > 1 else "battle")
streams = [p for k, f, l, p, pk in bl if k == RB_STREAM]
real = sum(pk for k, f, l, p, pk in bl if k == RB_STREAM)
raw = sum(len(p) for p in streams)
print(f"stream blocks {len(streams)} raw {raw/1e6:.1f} MB, writer packed {real/1e6:.2f} MB")
def run(name, comp, decomp=None):
    t = time.perf_counter(); outs = [comp(p) for p in streams]; tc = time.perf_counter() - t
    tot = sum(len(o) for o in outs)
    td = 0
    if decomp:
        t = time.perf_counter()
        for o, p in zip(outs, streams):
            d = decomp(o)
        td = time.perf_counter() - t
        assert d == streams[-1]
    print(f"{name:<34}{tot/1e6:9.2f} MB {tot/real:6.3f}x  comp {raw/1e6/tc:8.1f} MB/s  dec {raw/1e6/td if td else 0:8.1f} MB/s", flush=True)
run("zlib 6 wbits11 (writer-like)", lambda p: (lambda c: c.compress(p) + c.flush())(zlib.compressobj(6, zlib.DEFLATED, 11)), zlib.decompress)
for lv in (1, 6, 9):
    run(f"zlib {lv} 32K", lambda p, lv=lv: zlib.compress(p, lv), zlib.decompress)
for lv in (1, 2, 3, 5, 7, 9, 12, 15, 17, 19, 22):
    c = zstandard.ZstdCompressor(level=lv); d = zstandard.ZstdDecompressor()
    run(f"zstd {lv}", c.compress, d.decompress)
for lv in (-1, -5):
    c = zstandard.ZstdCompressor(level=lv); d = zstandard.ZstdDecompressor()
    run(f"zstd {lv} (fast)", c.compress, d.decompress)
run("lz4 default", lambda p: lz4.block.compress(p), lambda o: lz4.block.decompress(o))
run("lz4 hc 9", lambda p: lz4.block.compress(p, mode='high_compression', compression=9), lambda o: lz4.block.decompress(o))
for q in (5, 9, 11):
    run(f"brotli q{q} w22", lambda p, q=q: brotli.compress(p, quality=q, lgwin=22), brotli.decompress)
for pre in (1, 6, 9):
    run(f"lzma preset {pre}", lambda p, pre=pre: lzma.compress(p, preset=pre), lzma.decompress)
filt = [{"id": lzma.FILTER_LZMA2, "preset": 6, "lc": 0, "lp": 2, "pb": 2}]
run("lzma 6 lc0 lp2 pb2", lambda p: lzma.compress(p, format=lzma.FORMAT_RAW, filters=filt), lambda o: lzma.decompress(o, format=lzma.FORMAT_RAW, filters=filt))
