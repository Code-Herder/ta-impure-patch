"""rules_air.py -- the exact integer rules of the air step, scored on air.npz.

    rules_air.py air.npz
"""
import sys
import math
import numpy as np
from airpairs import Pairs, s32, asr16_mul
from airseries import AIR_TYPES

P = Pairs(sys.argv[1])
c, p = P.c, P.p
layer2 = (c["layer"] & 3) == 2
lp2 = (p["layer"] & 3) == 2
print("pairs", P.n, "layer2 now", int(layer2.sum()), "layer2 both", int((layer2 & lp2).sum()))

def report(name, ok, mask, changed=None):
    out = [f"{name:38s}"]
    for t in [None] + sorted(AIR_TYPES):
        m = mask if t is None else mask & (P.typ == t)
        n = int(m.sum())
        if n == 0:
            continue
        out.append(f"{'all' if t is None else AIR_TYPES[t]}: {int((ok & m).sum())}/{n}")
    if changed is not None:
        m = mask & changed
        out.append(f"| on changes {int((ok & m).sum())}/{int(m.sum())}")
    print("  ".join(out))

both = layer2 & lp2
# R1: speed = isqrt(vx^2+vy^2+vz^2)
sq = c["vx"] ** 2 + c["vy"] ** 2 + c["vz"] ** 2
isq = np.array([math.isqrt(int(v)) for v in sq], np.int64)
report("speed = isqrt(|v|^2)", isq == c["sp"], layer2, c["sp"] != p["sp"])
# R2: A_s = (A_s(t-1)*0xF333 >> 16) + (v - v(t-1))
for a, v in (("ax", "vx"), ("ay", "vy"), ("az", "vz")):
    pred = s32(asr16_mul(p[a], 0xF333) + (c[v] - p[v]))
    report(f"{a} = {a}(t-1)*0.95 + d{v}", pred == c[a], layer2 & lp2, c[a] != p[a])
# R5: y = y(t-1) + vy
report("y = y(t-1) + vy", c["y"] == p["y"] + c["vy"], layer2 & lp2, c["y"] != p["y"])
report("x = x(t-1) + vx", c["x"] == p["x"] + c["vx"], layer2 & lp2, c["x"] != p["x"])
report("heading = h(t-1) + tr", (c["h"] == (p["h"] + c["tr"]) % 65536), layer2 & lp2, c["h"] != p["h"])
# what writes when layer is not 2
report("not layer2: v == 0", (c["vx"] == 0) & (c["vy"] == 0) & (c["vz"] == 0), ~layer2)
report("not layer2: ax unchanged", c["ax"] == p["ax"], ~layer2)
report("not layer2: bank unchanged", c["bank"] == p["bank"], ~layer2)
report("roll == 0", c["roll"] == 0, layer2)
report("bank changes", c["bank"] != p["bank"], layer2 & lp2)
