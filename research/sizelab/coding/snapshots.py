"""The snapshot blocks under the candidate compressors (each alone: a seek starts there)."""
import zlib, lzma, time, zstandard
from common import *
bl = load("battle")
snaps = [(p, pk) for k, f, l, p, pk in bl if k == RB_SNAPSHOT]
raw = sum(len(p) for p, _ in snaps); real = sum(pk for _, pk in snaps)
print(f"{len(snaps)} snapshots raw {raw/2**20:.2f} MiB, writer {real/2**20:.3f} MiB")
for lv in (3, 9, 19):
    c = zstandard.ZstdCompressor(level=lv); t = time.perf_counter()
    tot = sum(len(c.compress(p)) for p, _ in snaps); dt = time.perf_counter() - t
    print(f"zstd {lv:>2}: {tot/2**20:.3f} MiB  {dt/len(snaps)*1e3:.0f} ms a snapshot (64-bit)")
t = time.perf_counter(); tot = sum(len(lzma.compress(p, preset=6)) for p, _ in snaps)
print(f"lzma 6: {tot/2**20:.3f} MiB  {(time.perf_counter()-t)/len(snaps)*1e3:.0f} ms a snapshot")
