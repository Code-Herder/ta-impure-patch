"""classstats.py -- per unit class: which field lanes change, and how often each of frame2's
engine-rule candidates (and prev / lin) is exact.  Run in a folder holding units.npz:

    classstats.py [out.json]

Classes by behaviour, per unit type (+0xA6) [INFERRED classification, not the def's flags]:
    structure  +0x110 bit 29 (no mover)
    air        some unit of the type had the move object's layer (+0x146 & 3) = 2
    floater    a mover whose type's altitude (+0x6E) is one value over the whole recording
    ground     every other mover
A unit-iteration counts when the slot is live at t-1 and t and not created at t (lin needs t-2
live as well; it is counted where it is). The rule candidates are frame2.Rules.candidate itself,
fed the true previous and current lanes, so "exact" is exactly when the codec's rule predicts.
"""
import json
import sys
from collections import defaultdict

import numpy as np

import frame2
import sim
from frame import Lanes, LANES

ln = Lanes(LANES["field"]())
R = frame2.Rules(ln)
I = ln.index
M = 0x118
NAMES = {0x64: "pitch", 0x66: "heading", 0x68: "roll", 0x6A: "x", 0x6E: "y (altitude)", 0x72: "z",
         M + 0x08: "mover vx", M + 0x0C: "mover vy", M + 0x10: "mover vz", M + 0x20: "mover speed",
         M + 0x24: "mover turn rate", M + 0x26: "mover +0x26", M + 0x28: "mover +0x28",
         M + 0x2A: "mover stamp", M + 0x2E: "mover layer/bits", M + 0x2F: "mover +0x2F",
         M + 0x00: "mover +0x00", M + 0x04: "mover +0x04", M + 0x14: "mover +0x14", M + 0x18: "mover +0x18",
         M + 0x1C: "mover +0x1C",
         0x18: "w1 reload", 0x34: "w2 reload", 0x50: "w3 reload", 0x1A: "w1 aim h", 0x1C: "w1 aim p",
         0xFA: "+0xFA countdown", 0xFB: "+0xFB..FE", 0x10A: "water state", 0x110: "state bits",
         0x76: "footprint x", 0x78: "footprint z", 0xA6: "type", 0x92: "def ptr", 0x108: "hp",
         0x7A: "sight x", 0x7C: "sight z", 0x82: "sort bucket", 0x104: "build fraction"}
nm = lambda o: NAMES.get(o, f"+0x{o:03X}")

z = sim.load("units.npz")
lanes = lambda S: ln.split(S)
L_TYPE, L_STATE, L_LAYER, L_Y = I[0xA6], I[0x110], I[M + 0x2E], I[0x6E]

# pass 1: per type
tinfo = defaultdict(lambda: {"nomover": 0, "mover": 0, "air": False, "ymin": 1 << 40, "ymax": -1,
                             "first_live": 0, "uits": 0})
created_types = defaultdict(int)
first = True
for tk in sim.ticks(z):
    lv = np.flatnonzero(tk.live)
    if not len(lv):
        continue
    L = lanes(tk.S[lv])
    ty = L[:, L_TYPE]
    for s in tk.created:
        created_types[int(ln.split(tk.S[[s]])[0, L_TYPE])] += 1
    nomv = (L[:, L_STATE] >> 29) & 1
    for t in np.unique(ty):
        k = ty == t
        d = tinfo[int(t)]
        d["uits"] += int(k.sum())
        d["nomover"] += int(nomv[k].sum())
        mv = k & (nomv == 0)
        d["mover"] += int(mv.sum())
        if mv.any():
            d["air"] |= bool(((L[mv, L_LAYER] & 3) == 2).any())
            y = L[mv, L_Y]
            y = np.where(y >= 1 << 31, y - (1 << 32), y)
            d["ymin"] = min(d["ymin"], int(y.min()))
            d["ymax"] = max(d["ymax"], int(y.max()))
        if first:
            d["first_live"] = int(k.sum())
    first = False


def cls_of(d):
    if d["mover"] == 0:
        return "structure"
    if d["air"]:
        return "air"
    if d["ymin"] == d["ymax"]:
        return "floater"
    return "ground"


tclass = {t: cls_of(d) for t, d in tinfo.items()}
CL = ["ground", "floater", "air", "structure"]
cmap = np.full(65536, 3, np.int64)
for t, c in tclass.items():
    cmap[t] = CL.index(c)

# pass 2
chg = np.zeros((4, ln.n), np.int64)
pairs = np.zeros(4, np.int64)
stats = defaultdict(lambda: np.zeros(6, np.int64))   # total, exact, changed, changed&exact, prev exact, lin exact (of lin-able)
linable = defaultdict(lambda: np.zeros(2, np.int64))  # lin-able total, lin exact
Lp = np.zeros((sim.SLOTS, ln.n), np.int64)
Lpp = np.zeros((sim.SLOTS, ln.n), np.int64)
live1 = np.zeros(sim.SLOTS, bool)
live2 = np.zeros(sim.SLOTS, bool)
s32 = frame2.s32
for tk in sim.ticks(z):
    lv = np.flatnonzero(tk.live)
    Lall = np.zeros((sim.SLOTS, ln.n), np.int64)
    if len(lv):
        Lall[lv] = lanes(tk.S[lv])
    ok = tk.live & live1
    if tk.created:
        ok[tk.created] = False
    idx = np.flatnonzero(ok)
    if len(idx):
        L0, L1, L2 = Lall[idx], Lp[idx], Lpp[idx]
        ok2 = live2[idx]
        c = cmap[L0[:, L_TYPE]]
        mover = ((L0[:, L_STATE] >> 29) & 1) == 0
        ch = L0 != L1
        for k in range(4):
            sel = c == k
            pairs[k] += int(sel.sum())
            chg[k] += ch[sel].sum(axis=0)
        blocked = (L0[:, I[M + 0x2E]] & 4) != 0
        for lane in R.order + [I[0x6E]]:
            act = L0[:, lane]
            mod = ln.mod[lane]
            if lane == I[0x6E]:
                pred = (L1[:, lane] + s32(L0[:, I[M + 0x0C]])) % mod      # y <- vy (not a frame2 rule)
            else:
                pred = R.candidate(lane, L1, L0, tk.gametime) % mod
            ex = pred == act
            chd = act != L1[:, lane]
            linex = ((2 * L1[:, lane] - L2[:, lane]) % mod) == act
            o = ln.table[lane][0]
            groups = [("all", np.ones(len(idx), bool))]
            if lane in (I[0x6A], I[0x72], I[0x6E]):
                groups += [("blocked", blocked), ("free", ~blocked)]
            for k in range(4):
                base = (c == k) & (mover if o >= 0x118 or lane in (I[0x66], I[0x6A], I[0x72], I[0x6E]) else True)
                for gname, g in groups:
                    s = base & g
                    key = (CL[k], nm(o), gname)
                    stats[key] += [s.sum(), (s & ex).sum(), (s & chd).sum(), (s & chd & ex).sum(),
                                   (s & ~chd).sum(), 0]
                    linable[key] += [(s & ok2).sum(), (s & ok2 & linex).sum()]
    Lpp[:] = Lp
    Lp[:] = Lall
    live2 = live1 & tk.live
    live1 = tk.live.copy()
    if tk.created:
        live2[tk.created] = False

out = {"types": {str(t): {**tinfo[t], "class": tclass[t], "created_in_stream": created_types.get(t, 0)}
                 for t in sorted(tinfo)},
       "pairs": dict(zip(CL, pairs.tolist())),
       "lane_changes": {CL[k]: {f"+0x{ln.table[j][0]:03X}/{ln.table[j][1]} {nm(ln.table[j][0])}": int(chg[k, j])
                                for j in range(ln.n) if chg[k, j]} for k in range(4)},
       "rules": [{"class": k[0], "lane": k[1], "group": k[2], "n": int(v[0]), "exact": int(v[1]),
                  "changed": int(v[2]), "changed_exact": int(v[3]), "unchanged": int(v[4]),
                  "lin_n": int(linable[k][0]), "lin_exact": int(linable[k][1])}
                 for k, v in stats.items() if v[0]]}
json.dump(out, open(sys.argv[1] if len(sys.argv) > 1 else "classstats.json", "w"), indent=1)
print("types:")
for t in sorted(tinfo):
    d = tinfo[t]
    print(f"  type {t:4d} {tclass[t]:9s} first-tick {d['first_live']:4d} created {created_types.get(t,0):4d} "
          f"unit-its {d['uits']:8d} y {d['ymin']}..{d['ymax']}")
print("pairs", dict(zip(CL, pairs.tolist())))
for r in out["rules"]:
    if r["group"] == "all" or r["n"]:
        print(f"{r['class']:9s} {r['lane']:16s} {r['group']:7s} n {r['n']:9d} rule exact {r['exact']/r['n']:7.2%} "
              f"changed {r['changed']:9d} changed&exact {r['changed_exact']/max(r['changed'],1):7.2%} "
              f"lin exact {r['lin_exact']/max(r['lin_n'],1):7.2%}")
