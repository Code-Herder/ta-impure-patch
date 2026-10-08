"""p2: with the identity map, which dwords of a surviving projectile change per tick, and how well
three predictors do: prev (the same projectile one tick ago), linear (2 prev - prev2) and, for the
position, prev + the new velocity + last tick's drift. Per weapon ID too."""
import sys, struct, collections
import numpy as np
from pool import iterations, match

M32 = np.uint64(0xFFFFFFFF)
def sdiff(a, b):
    return ((a.astype(np.int64) - b.astype(np.int64) + 2**31) % 2**32) - 2**31

prev = None
hist = None      # state at T-1 of each live record at T (rows), valid mask
n_surv = 0
chg = np.zeros(27, np.int64)
nz_lin = np.zeros(27, np.int64)
nz_phys = np.zeros(3, np.int64)
n_lin = 0
per_w = collections.defaultdict(lambda: np.zeros(27, np.int64))
per_w_n = collections.Counter()
per_w_lin = collections.defaultdict(lambda: np.zeros(27, np.int64))
per_w_life = collections.defaultdict(list)
born = {}
agree = collections.Counter()
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    newhist = np.zeros((c, 27), np.uint32); newvalid = np.zeros(c, bool)
    if prev is not None:
        Q, qc = prev["pool"], prev["count"]
        mp = match(Q, qc, P, c)
        s = mp >= 0
        idx = np.nonzero(s)[0]
        n_surv += len(idx)
        if len(idx):
            cur = P[idx]; old = Q[mp[idx]]
            d = cur != old
            chg += d.sum(axis=0)
            newhist[idx] = old; newvalid[idx] = True
            hv = hist_valid[mp[idx]]
            if hv.any():
                ii = np.nonzero(hv)[0]
                o2 = hist[mp[idx][ii]]
                c1 = cur[ii].astype(np.int64); o1 = old[ii].astype(np.int64); o0 = o2.astype(np.int64)
                lin = (2 * o1 - o0) % 2**32
                nz = lin != c1
                nz_lin += nz.sum(axis=0); n_lin += len(ii)
                # physics on position: pos = prev + vel_new + (prev - prev2 - vel_prev)
                for a, (pd, vd) in enumerate(((1, 7), (2, 8), (3, 9))):
                    pred = (o1[:, pd] + c1[:, vd] + (o1[:, pd] - o0[:, pd] - o1[:, vd])) % 2**32
                    nz_phys[a] += (pred != c1[:, pd]).sum()
                for r in range(len(ii)):
                    w = int(cur[ii[r], 0])
                    per_w_lin[w] += nz[r]
            for r in range(len(idx)):
                w = int(cur[r, 0]); per_w[w] += d[r]; per_w_n[w] += 1
        if prev["snap_before"]:
            pass
    hist, hist_valid = newhist, newvalid
    if it["snap_before"]:
        hist_valid[:] = False
    prev = it
print("survivor transitions", n_surv, "with two ticks of history", n_lin)
names = ["weapon", "pos x", "pos y", "pos z", "tail x", "tail y", "tail z", "vel x", "vel y", "vel z",
         "tgt x", "tgt y", "tgt z", "spin|heading", "pitch|speed.lo", "speed.hi|dist.lo", "dist.hi|spawn.lo",
         "spawn.hi|death.lo", "death.hi|smoke.lo", "smoke.hi|target.lo", "target.hi|attacker.lo",
         "attacker.hi|quarry.lo", "quarry.hi|cell", "5C|5E", "burst|piece", "flame|owner|idx.lo",
         "idx.hi|flags|pad"]
for d in range(27):
    print(f"d{d:2d} +0x{4*d:02X} {names[d]:24s} changed {chg[d]/max(n_surv,1)*100:6.2f} %   linear residual nonzero {nz_lin[d]/max(n_lin,1)*100:6.2f} %")
print("physics position residual nonzero %:", (nz_phys / max(n_lin, 1) * 100).round(2))
print()
print("per weapon: survivor transitions, % changed per dword (d1..d9, d13..d15, d25, d26), linear nonzero %")
for w, n in per_w_n.most_common(20):
    ch = per_w[w] / n * 100
    ln = per_w_lin[w] / n * 100
    sel = [1,2,3,4,5,6,7,8,9,13,14,15,16,17,18,25,26]
    print(f"weapon {w:5d} n {n:7d} ch " + " ".join(f"{ch[k]:3.0f}" for k in sel))
    print(f"{'':20s} ln " + " ".join(f"{ln[k]:3.0f}" for k in sel))
