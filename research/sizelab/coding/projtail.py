"""Is a projectile's tail (+0x10..+0x1B) its position (+0x04..+0x0F) of the iteration before?
Over the deltas of the first 3,000 iterations: dwords 4..6 at t against dwords 1..3 at t-1."""
import struct, collections
import numpy as np
import reencode as R
from common import *
T = R.T
cur = {}; prev = {}; n = 0; c = collections.Counter(); seen = set()
for k, f, l, p, pk in load("battle"):
    if k != RB_STREAM: continue
    for kind, b in records(p):
        if kind == T.RK_TICK:
            prev = {i: v.copy() for i, v in cur.items()}; n += 1; seen = set()
        elif kind == T.RK_PROJ:
            cur[struct.unpack_from("<H", b)[0]] = np.frombuffer(b[4:], dtype=np.uint32).copy()
        elif kind == T.RK_PROJ_DELTA:
            i = struct.unpack_from("<H", b)[0]
            img = cur.get(i, np.zeros(27, np.uint32)).copy()
            for k2, d in enumerate(R.bits_list(b[2:6])):
                img[d] = struct.unpack_from("<I", b, 8 + 4 * k2)[0]
            cur[i] = img
            nb = cur.get(i - 1)
            if nb is not None and i - 1 in seen:
                eq = int(np.sum(img == nb)); c["vs record i-1 this iteration: deltas"] += 1; c["vs record i-1: equal dwords"] += eq
                c["vs record i-1: >= 3 bytes of the high 3 bytes equal (dwords)"] += int(np.sum((img >> 8) == (nb >> 8)))
            q2 = prev.get(i)
            if q2 is not None:
                c["vs own last: high-3-bytes equal (dwords)"] += int(np.sum((img >> 8) == (q2 >> 8)))
            seen.add(i)
            q = prev.get(i)
            if q is not None:
                c["deltas"] += 1
                c["tail == last position"] += int(np.array_equal(img[4:7], q[1:4]))
                c["tail == position"] += int(np.array_equal(img[4:7], img[1:4]))
        if n > 3000: break
    if n > 3000: break
print(dict(c))
