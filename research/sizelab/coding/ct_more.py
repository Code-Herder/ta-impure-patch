"""lzma, brotli and zstd 19 on the sectioned container's 30-iteration blocks (the dump), each alone."""
import struct, lzma, time, brotli, zstandard
idx = open("ct-N30.idxbin", "rb").read(); data = open("ct-N30.bin", "rb").read()
n = struct.unpack_from("<I", idx)[0]
blocks = []
for i in range(n):
    k, f, l, raw, pk, off = struct.unpack_from("<6I", idx, 4 + 24 * i)
    if k == 2: blocks.append(data[off:off + raw])
tot_raw = sum(len(b) for b in blocks)
filt = [{"id": lzma.FILTER_LZMA2, "preset": 6, "lc": 0, "lp": 2, "pb": 2}]
for label, f in (("lzma 6", lambda b: lzma.compress(b, preset=6)),
                 ("lzma 6 lc0 lp2 pb2", lambda b: lzma.compress(b, format=lzma.FORMAT_RAW, filters=filt)),
                 ("brotli q5 w22", lambda b: brotli.compress(b, quality=5, lgwin=22)),
                 ("brotli q9 w22", lambda b: brotli.compress(b, quality=9, lgwin=22))):
    t = time.perf_counter(); tot = sum(len(f(b)) for b in blocks); dt = time.perf_counter() - t
    print(f"{label:<22}{tot/2**20:8.2f} MiB  {tot_raw/1e6/dt:6.1f} MB/s (64-bit)", flush=True)
