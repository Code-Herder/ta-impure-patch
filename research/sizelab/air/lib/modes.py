"""modes.py -- each air rule's exactness per aircraft mode, fed the true previous and current
values (as classstats.py does for frame2).  Modes, from the record [INFERRED from behaviour]:
    landed      the mover's layer is not 2 (the air step zeroes v, speed and turn rate)
    takeoff/landing  the layer changed between t-1 and t
    hover       airborne, speed below 1 wu/tick
    turn        airborne, turn rate nonzero
    straight    airborne, turn rate zero
  and separately: climbing/descending (vy != 0), dying (+0x110 bit 14).
    modes.py air.npz [out.json]
"""
import sys, json
import numpy as np
from airpairs import Pairs, FIELDS, field
from airseries import AIR_TYPES
import frame3 as f3, airdefs as D

P = Pairs(sys.argv[1])
c, p, pp = P.c, P.p, P.pp
tt = f3.TypeTable(D.DEF, D.GRAVITY, D.SEA)
typ = P.typ
z = np.load(sys.argv[1]); o = np.lexsort((z["tick"], z["slot"]))
tk, sl, cr = z["tick"][o], z["slot"][o], z["created"][o]
i3 = np.maximum(P.i - 3, 0)
ok3 = (P.i >= 3) & (sl[i3] == P.slot) & (tk[i3] == P.tick - 3) & ~cr[np.maximum(P.i - 2, 0)] & ~cr[np.maximum(P.i - 1, 0)]
F3 = {k: field(P.S[i3], *FIELDS[k]) for k in ("vx", "vy", "vz", "h", "layer")}
L2c = (c["layer"] & 3) == 2
L2p = (p["layer"] & 3) == 2
L2pp = (pp["layer"] & 3) == 2
# -- rule predictions with true inputs --
sq = lambda a: f3.clip(a)
pred = {}
pred["speed"] = np.where(L2c, f3.isqrt64(sq(c["vx"]) ** 2 + sq(c["vy"]) ** 2 + sq(c["vz"]) ** 2), 0)
for a, v in (("ax", "vx"), ("ay", "vy"), ("az", "vz")):
    pred[a] = np.where(L2c, f3.s32(f3.mul_asr16(p[a], 0xF333) + c[v] - p[v]), p[a])
pred["bank"] = np.where(L2c, f3.s16(f3.bank_angle(c["ax"], c["az"], c["h"], tt.bank[typ], tt.q)), p["bank"])
pred["y"] = f3.s32(p["y"] + c["vy"])
kx, ky, kz = f3.known_part(tt, typ, p["h"], p["vx"], p["vy"], p["vz"])
qx, _, qz = f3.known_part(tt, typ, pp["h"], pp["vx"], pp["vy"], pp["vz"])
rx, _, rz = f3.known_part(tt, typ, F3["h"], F3["vx"], F3["vy"], F3["vz"])
ok1 = P.has_pp & L2p & L2pp
ok2 = ok1 & ok3 & ((F3["layer"] & 3) == 2)
a1x, a1z = np.where(ok1, p["vx"] - qx, 0), np.where(ok1, p["vz"] - qz, 0)
a2x, a2z = np.where(ok2, pp["vx"] - rx, 0), np.where(ok2, pp["vz"] - rz, 0)
pred["v (known + A(t-1))"] = (np.where(L2c, kx + a1x, 0), np.where(L2c, kz + a1z, 0))
px, pz = f3.rot_extrapolate(a1x, a1z, a2x, a2z)
pred["v (known + A turned)"] = (np.where(L2c, kx + px, 0), np.where(L2c, kz + pz, 0))
c1 = np.where((p["sp"] & ~3) < 0x40000, 0x10000, p["sp"] >> 2)
c2 = np.where((pp["sp"] & ~3) < 0x40000, 0x10000, pp["sp"] >> 2)
pred["vy"] = np.where(L2c, np.where(P.has_pp & (p["vy"] == c2), c1, np.where(P.has_pp & (p["vy"] == -c2), -c1, 0)), 0)
S, Sp = P.S[P.i], P.S[P.i - 1]
fw, fh = field(S, 0x7E, 2, True), field(S, 0x80, 2, True)
fx, fz = field(S, 0x76, 2, False), field(S, 0x78, 2, False)
fxp, fzp = field(Sp, 0x76, 2, False), field(Sp, 0x78, 2, False)
pred["footprint"] = (((c["x"] - (fw << 19) + 0x80000) >> 20) & 0xFFFF, ((c["z"] - (fh << 19) + 0x80000) >> 20) & 0xFFFF)
yy = np.maximum(c["y"], (tt.sea + 1) << 16)
hg = np.clip(f3.s16(yy >> 16) + tt.sight_h[typ], 0, 255)
pred["sight x"] = (c["x"] >> 21) & 0xFFFF
pred["sight z"] = (((c["z"] >> 16) - (hg >> 1)) >> 5) & 0xFFFF
pred["v: either candidate"] = None
moved = (fx != fxp) | (fz != fzp) | ((c["layer"] & 3) != (p["layer"] & 3))
pred["m26"] = np.where(moved, (P.gt - 1) & 0xFFFF, p["m26"])
act = {"speed": c["sp"], "ax": c["ax"], "ay": c["ay"], "az": c["az"], "bank": c["bank"], "y": c["y"],
       "v (known + A(t-1))": (c["vx"], c["vz"]), "v (known + A turned)": (c["vx"], c["vz"]), "vy": c["vy"],
       "footprint": (fx, fz), "sight x": field(S, 0x7A, 2, False), "sight z": field(S, 0x7C, 2, False), "m26": c["m26"]}
prev_ = {"v (known + A(t-1))": (p["vx"], p["vz"]), "v (known + A turned)": (p["vx"], p["vz"]),
         "footprint": (fxp, fzp)}
pv1 = {"sight x": field(Sp, 0x7A, 2, False), "sight z": field(Sp, 0x7C, 2, False)}
# -- modes --
modes = {
    "landed": ~L2c & ~L2p,
    "takeoff/landing": L2c != L2p,
    "hover (<1 wu/tick)": L2c & L2p & (c["sp"] < 0x10000),
    "turning": L2c & L2p & (c["sp"] >= 0x10000) & (c["tr"] != 0),
    "straight": L2c & L2p & (c["sp"] >= 0x10000) & (c["tr"] == 0),
    "climb/descent (vy != 0)": L2c & L2p & (c["vy"] != 0),
    "dying (+0x110 bit 14)": ((c["state"] >> 14) & 1) == 1,
}
out = {}
print(f"{'rule':24s} " + " ".join(f"{k[:18]:>20s}" for k in modes))
print(f"{'unit-iterations':24s} " + " ".join(f"{int(m.sum()):>20d}" for m in modes.values()))
for name in pred:
    if pred[name] is None:
        a, b = pred["v (known + A(t-1))"], pred["v (known + A turned)"]
        ok = ((a[0] == c["vx"]) & (a[1] == c["vz"])) | ((b[0] == c["vx"]) & (b[1] == c["vz"]))
        ch = (c["vx"] != p["vx"]) | (c["vz"] != p["vz"])
        out[name] = {k: (int((ok & m).sum()), int(m.sum()), int((ch & m).sum()), int((ok & ch & m).sum())) for k, m in modes.items()}
        print(f"{name:24s} " + " ".join(f"{(r[0] / r[1] if r[1] else float('nan')):>19.4f} " for r in out[name].values()))
        continue
    pr, ac = pred[name], act[name]
    if isinstance(pr, tuple):
        ok = (pr[0] == ac[0]) & (pr[1] == ac[1])
        ch = (ac[0] != prev_[name][0]) | (ac[1] != prev_[name][1])
    else:
        ok = pr == ac
        pv = {"speed": p["sp"], "ax": p["ax"], "ay": p["ay"], "az": p["az"], "bank": p["bank"], "y": p["y"],
              "vy": p["vy"], "m26": p["m26"], **pv1}[name]
        ch = ac != pv
    row = {}
    for k, m in modes.items():
        n = int(m.sum())
        row[k] = (int((ok & m).sum()), n, int((ch & m).sum()), int((ok & ch & m).sum()))
    out[name] = row
    print(f"{name:24s} " + " ".join(f"{(r[0] / r[1] if r[1] else float('nan')):>19.4f} " for r in row.values()))
if len(sys.argv) > 2:
    json.dump(out, open(sys.argv[2], "w"), indent=1)
