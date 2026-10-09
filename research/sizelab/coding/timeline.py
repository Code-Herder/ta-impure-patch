"""Packed bytes per 30 s of GameTime: today's file (the writer's blocks) against the sectioned
container's 30-iteration blocks under zstd 3 (each alone)."""
import struct, collections, zstandard
from common import *
bl = load("battle")
today = collections.Counter()
for k, f, l, p, pk in bl:
    if k == RB_STREAM: today[f // 900] += pk + 32
idx = open("ct-N30.idxbin", "rb").read(); data = open("ct-N30.bin", "rb").read()
n = struct.unpack_from("<I", idx)[0]
z3 = zstandard.ZstdCompressor(level=3)
new = collections.Counter(); raw = collections.Counter()
for i in range(n):
    k, f, l, r, pk, off = struct.unpack_from("<6I", idx, 4 + 24 * i)
    if k != 2: continue
    b = data[off:off + r]
    new[f // 900] += len(z3.compress(b)) + 32; raw[f // 900] += r
print(f"{'GameTime':<12}{'today MiB':>10}{'new MiB':>10}{'ratio':>7}{'new raw MiB':>13}")
for w in sorted(today):
    print(f"{w*900:>5}-{w*900+899:<6}{today[w]/2**20:>10.2f}{new[w]/2**20:>10.2f}{new[w]/today[w]:>7.2f}{raw[w]/2**20:>13.1f}")
tail = [w for w in sorted(today) if w * 900 >= 4500 and w * 900 + 899 <= 7199]
t_today = sum(today[w] for w in tail) / len(tail); t_new = sum(new[w] for w in tail) / len(tail)
print(f"tail (GameTime 4500-7199, ~400 units): today {t_today/2**20:.2f} MiB / 30 s = {t_today*120/2**20:.0f} MiB an hour; new {t_new/2**20:.2f} MiB / 30 s = {t_new*120/2**20:.0f} MiB an hour")
