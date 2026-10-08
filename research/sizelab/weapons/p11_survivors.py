"""p11: what the survivors' residuals are, under the predictors the per-block choice settles on
(d1-d3 phys, d4-d6 and d8 lin, d25 the stamp, the rest prev): per weapon and template/flight, the
share of transitions with any nonzero residual, the nonzero count per column group, and the LEB
bytes of the nonzero residuals."""
import sys, struct, collections
import numpy as np
from pool import iterations
import codec as C
import codec3 as C3

MODES = {d: "prev" for d in range(27)}
MODES.update({1: "phys", 2: "phys", 3: "phys", 4: "lin", 5: "lin", 6: "lin", 8: "lin", 25: "idx25"})
GROUPS = {"pos": [1, 2, 3], "tail": [4, 5, 6], "vel": [7, 8, 9], "34..3F angles/speed": [13, 14, 15],
          "40..4B times": [16, 17, 18], "5C/5E ground": [23], "60 burst/piece": [24],
          "64..6A flame/stamp/flags": [25, 26], "other": [0, 10, 11, 12, 19, 20, 21, 22]}
enc = C3.Codec3(True)
st = collections.defaultdict(collections.Counter)
for it in iterations(sys.argv[1], want_recs=False):
    Q, qc = enc.Q, enc.qc
    P = it["pool"]
    ctl, rs, rn, rt, refs = enc.encode(P, it["count"], it["hw"], it["snap_before"])
    ns = len(rs[0]["prev"])
    if not ns: continue
    R = np.zeros((ns, 27), np.uint32)
    for col in range(27):
        R[:, col] = rs[col][MODES[col]]
    for j in range(ns):
        w = int(P[j, 0]); tp = struct.unpack_from("<H", P[j].tobytes(), 0x60)[0] > 0
        k = (w, "template" if tp else "flight")
        s = st[k]; s["n"] += 1
        nz = R[j] != 0
        s["any"] += bool(nz.any())
        for g, cols in GROUPS.items():
            s[g] += int(nz[cols].sum())
        s["bytes"] += sum(len(C.leb(C.zz(np.array([R[j, c]], np.uint32)))) for c in np.nonzero(nz)[0])
tot = collections.Counter()
for k, s in st.items():
    for g, v in s.items(): tot[g] += v
print(f"{'weapon/kind':16s} {'trans':>7s} {'any nz %':>8s} {'nz bytes':>9s} " + " ".join(f"{g[:10]:>10s}" for g in GROUPS))
for k, s in sorted(st.items(), key=lambda x: -x[1]["bytes"]) + [(("all", ""), tot)]:
    if s["n"] < 50: continue
    print(f"{str(k[0])+' '+k[1]:16s} {s['n']:7d} {s['any']/s['n']*100:8.1f} {s['bytes']/1024:8.0f}K " + " ".join(f"{s[g]:10d}" for g in GROUPS))
