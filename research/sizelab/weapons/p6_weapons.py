"""p6: per weapon ID: records born, lifetime, burst templates (+0x60 > 0), how often the position
and velocity change, and the shot events' weapons; the motion class each weapon shows."""
import sys, struct, collections
import numpy as np
from pool import iterations, match
import codec as C

def f16(row, off): return struct.unpack_from("<H", row.tobytes(), off)[0]
def f32(row, off): return struct.unpack_from("<I", row.tobytes(), off)[0]
def s32(row, off): return struct.unpack_from("<i", row.tobytes(), off)[0]

born = collections.Counter(); life = collections.defaultdict(list); tmpl = collections.Counter()
tmpl_life = collections.defaultdict(list)
shotw = collections.Counter()
alive = {}     # id -> (weapon, birth tick, template?)
nextid = 0
prev = None; ids = []
velchg = collections.Counter(); trans = collections.Counter()
smoke = collections.Counter(); flame = collections.Counter()
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    for b in it["shots"]:
        w = b[0x19] | ((b[0x23] >> 4) << 8)
        shotw[w] += 1
    if prev is None:
        ids = []
        for j in range(c):
            ids.append(nextid); alive[nextid] = (int(P[j, 0]), it["tick"], f16(P[j], 0x60) > 0); nextid += 1
    else:
        Q, qc = prev["pool"], prev["count"]
        m = C.prefix_map(match(Q, qc, P, c), qc)
        newids = []
        used = set()
        for j in range(c):
            if j < len(m):
                i = ids[m[j]]; newids.append(i); used.add(int(m[j]))
                w = int(P[j, 0]); trans[w] += 1
                if not np.array_equal(P[j, 7:10], Q[m[j], 7:10]): velchg[w] += 1
                if f32(P[j], 0x4A) != f32(Q[m[j]], 0x4A): smoke[w] += 1
                if f16(P[j], 0x64) != f16(Q[m[j]], 0x64): flame[w] += 1
            else:
                i = nextid; nextid += 1
                alive[i] = (int(P[j, 0]), it["tick"], f16(P[j], 0x60) > 0)
                born[int(P[j, 0])] += 1
                if f16(P[j], 0x60) > 0: tmpl[int(P[j, 0])] += 1
                newids.append(i)
        for k in range(qc):
            if k not in used:
                w, t0, tp = alive.pop(ids[k])
                (tmpl_life if tp else life)[w].append(it["tick"] - t0)
        ids = newids
    prev = it
print("shot events by weapon:", shotw.most_common(12))
print(f"{'weapon':>7s} {'born':>7s} {'templ':>6s} {'life p50':>8s} {'tlife p50':>9s} {'surv trans':>10s} {'vel chg %':>9s} {'smoke %':>8s} {'flame %':>8s}")
for w, n in born.most_common(15):
    lf = np.median(life[w]) if life[w] else -1
    tl = np.median(tmpl_life[w]) if tmpl_life[w] else -1
    print(f"{w:7d} {n:7d} {tmpl[w]:6d} {lf:8.0f} {tl:9.0f} {trans[w]:10d} {velchg[w]/max(trans[w],1)*100:9.1f} {smoke[w]/max(trans[w],1)*100:8.1f} {flame[w]/max(trans[w],1)*100:8.1f}")
