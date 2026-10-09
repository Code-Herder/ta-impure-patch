"""p14: how many projectiles the predictors fly exactly from their first image alone, with no
correction (dead reckoning from the launch): per weapon and kind, the projectiles whose position
is exact at every tick of their life, and those whose every dword is (the +0x67 stamp taken from
the identity map, which the decoder has). The first transition's drift (wind, gravity) is taken
from the last one seen for the same weapon. Also the open-loop error of linear interpolation
between 10 Hz samples of the true position (the product contract's model for units)."""
import sys, struct, collections
import numpy as np
from pool import iterations, match
import codec as C

def s32(a): return (a.astype(np.int64) + 2**31) % 2**32 - 2**31

lastdrift = {}
lastaccel = {}
track = {}        # pid -> dict(state=pred row, prev=pred prev row, w, kind, exact_pos, exact_all, life)
ids = []; nextid = 0
done = collections.defaultdict(collections.Counter)
truth = {}        # pid -> list of true positions (for interpolation)
interr = collections.defaultdict(list)
prev = None
def predict_row(cur, prv, w, m_index):
    """one tick of dead reckoning: velocity x/z held, y linear, position += new velocity + drift,
    tail linear, the rest held, the stamp = its old index"""
    nxt = cur.astype(np.int64).copy()
    if prv is not None:
        p2 = prv.astype(np.int64)
        nxt[8] = 2 * cur[8].astype(np.int64) - p2[8]
        drift = cur[1:4].astype(np.int64) - p2[1:4] - cur[7:10].astype(np.int64)
        nxt[4:7] = 2 * cur[4:7].astype(np.int64) - p2[4:7]
    else:
        drift = lastdrift.get(w, np.zeros(3, np.int64))
        nxt[7:10] = cur[7:10].astype(np.int64) + lastaccel.get(w, np.zeros(3, np.int64))
    nxt[1:4] = cur[1:4].astype(np.int64) + nxt[7:10] + drift
    nxt[25] = (nxt[25] & 0x00FFFFFF) | ((m_index & 0xFF) << 24)
    nxt[26] = (nxt[26] & 0xFFFFFF00) | ((m_index >> 8) & 0xFF)
    return (nxt % 2**32).astype(np.uint32)

for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is None:
        ids = []
    else:
        Q, qc = prev["pool"], prev["count"]
        m = C.prefix_map(match(Q, qc, P, c), qc)
        alive = set()
        newids = []
        for j in range(c):
            if j < len(m):
                pid = ids[m[j]]; t = track[pid]
                # learn the weapon's drift from the truth
                if t["tprev"] is not None:
                    lastdrift[t["w"]] = s32(Q[m[j], 1:4].astype(np.int64)) * 0 + (
                        Q[m[j], 1:4].astype(np.int64) - t["tprev"][1:4].astype(np.int64)
                        - Q[m[j], 7:10].astype(np.int64))
                if t["tprev"] is not None:
                    lastaccel[t["w"]] = Q[m[j], 7:10].astype(np.int64) - t["tprev"][7:10].astype(np.int64)
                pr = predict_row(t["state"], t["prev"], t["w"], int(m[j]))
                t["prev"], t["state"] = t["state"], pr
                t["tprev"] = Q[m[j]].copy()
                if not np.array_equal(pr[1:4], P[j, 1:4]): t["pos"] = False
                if not np.array_equal(pr, P[j]): t["all"] = False
                t["life"] += 1
                truth[pid].append(P[j, 1:4].astype(np.int64))
                newids.append(pid); alive.add(pid)
            else:
                pid = nextid; nextid += 1
                w = int(P[j, 0]); tp = struct.unpack_from("<H", P[j].tobytes(), 0x60)[0] > 0
                track[pid] = dict(state=P[j].copy(), prev=None, tprev=None, w=w,
                                  kind="template" if tp else "flight", pos=True, all=True, life=0)
                truth[pid] = [P[j, 1:4].astype(np.int64)]
                newids.append(pid); alive.add(pid)
        for pid in ids:
            if pid not in alive:
                t = track.pop(pid)
                d = done[(t["w"], t["kind"])]
                d["n"] += 1; d["pos exact"] += t["pos"]; d["all exact"] += t["all"]
                d["ticks"] += t["life"]
                tr = np.array(truth.pop(pid))
                # linear interpolation between samples every 3rd tick of its life
                if t["kind"] == "flight" and len(tr) >= 4:
                    for k in range(0, len(tr) - 3, 3):
                        a, b = tr[k], tr[k + 3]
                        for s in (1, 2):
                            est = a + (b - a) * s / 3.0
                            interr[t["w"]].append(np.abs(est - tr[k + s]).max() / 65536.0)
        ids = newids
    if prev is None:
        for j in range(c):
            pid = nextid; nextid += 1
            track[pid] = dict(state=P[j].copy(), prev=None, tprev=None, w=int(P[j, 0]), kind="flight",
                              pos=True, all=True, life=0)
            truth[pid] = [P[j, 1:4].astype(np.int64)]
            ids.append(pid)
    prev = it
print(f"{'weapon kind':16s} {'shots':>6s} {'pos exact %':>11s} {'all exact %':>11s} {'mean life':>9s}")
tot = collections.Counter()
for (w, k), d in sorted(done.items(), key=lambda x: -x[1]["n"]):
    for kk, v in d.items(): tot[kk] += v
    if d["n"] < 20: continue
    print(f"{str(w)+' '+k:16s} {d['n']:6d} {d['pos exact']/d['n']*100:11.1f} {d['all exact']/d['n']*100:11.1f} {d['ticks']/d['n']:9.1f}")
print(f"{'all':16s} {tot['n']:6d} {tot['pos exact']/tot['n']*100:11.1f} {tot['all exact']/tot['n']*100:11.1f} {tot['ticks']/tot['n']:9.1f}")
print("linear interpolation between 10 Hz samples of the true position, error (wu):")
for w, v in sorted(interr.items(), key=lambda x: -len(x[1])):
    v = np.array(v)
    if len(v) < 50: continue
    print(f"   weapon {w:4d}: {len(v):7d} skipped ticks, p50 {np.percentile(v,50):.3f} p99 {np.percentile(v,99):.3f} max {v.max():.2f}")
