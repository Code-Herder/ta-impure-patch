"""Do projectile pool records move between indices from one iteration to the next?
For each delta (t, i), the record image after it against the images at t-1 of indices i, i+1, i-1:
how many dwords equal."""
import sys, struct, collections
import numpy as np
import reencode as R
from common import *
T = R.T
bl = load("battle")
cur = {}            # index -> np.array 27 dwords (current)
prev_tick = {}      # snapshot of cur at the end of the previous iteration (only the touched ones)
stats = collections.Counter()
hist = collections.defaultdict(collections.Counter)
n = 0
for k, f, l, p, pk in bl:
    if k != RB_STREAM: continue
    for kind, b in records(p):
        if kind == T.RK_TICK:
            prev = {i: v.copy() for i, v in cur.items()}
            n += 1
            if n > 3000: break
        elif kind == T.RK_PROJ:
            i = struct.unpack_from("<H", b)[0]
            cur[i] = np.frombuffer(b[4:], dtype=np.uint32).copy()
        elif kind == T.RK_PROJ_DELTA:
            i = struct.unpack_from("<H", b)[0]
            mask = b[2:6]
            img = cur.get(i, np.zeros(27, np.uint32)).copy()
            for k2, d in enumerate(R.bits_list(mask)):
                img[d] = struct.unpack_from("<I", b, 8 + 4 * k2)[0]
            cur[i] = img
            for off in (0, 1, -1, 2):
                q = prev.get(i + off)
                if q is not None:
                    hist[off][int(np.sum(q == img))] += 1
            stats["deltas"] += 1
    if n > 3000: break
print("deltas", stats["deltas"])
for off in (0, 1, -1, 2):
    h = hist[off]; tot = sum(h.values())
    print(f"index {off:+d}: mean equal dwords {sum(k*v for k,v in h.items())/tot:.1f}; >=20 equal: {sum(v for k,v in h.items() if k>=20)/tot:.2%}")
