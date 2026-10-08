import numpy as np
from airpairs import Pairs, s32
from airseries import AIR_TYPES
import airdefs as D
from airstep import known_part

P = Pairs("air.npz")
c, p, pp = P.c, P.p, P.pp
m = ((c["layer"] & 3) == 2) & ((p["layer"] & 3) == 2) & P.has_pp & ((pp["layer"] & 3) == 2)
v2x, v2y, v2z, over = known_part(P.typ, p["h"], p["vx"], p["vy"], p["vz"])
q2x, q2y, q2z, _ = known_part(P.typ, pp["h"], pp["vx"], pp["vy"], pp["vz"])
Ax, Az = c["vx"] - v2x, c["vz"] - v2z
Apx, Apz = p["vx"] - q2x, p["vz"] - q2z
acc = D.per_type(P.typ, "acc")
def stat(name, rx, rz):
    rx, rz = rx[m], rz[m]
    print(f"{name:28s} vx exact {(rx==0).mean():.3f} |r|<=1 {(abs(rx)<=1).mean():.3f} p90 {np.percentile(abs(rx),90):7.0f} | "
          f"vz exact {(rz==0).mean():.3f} |r|<=1 {(abs(rz)<=1).mean():.3f} p90 {np.percentile(abs(rz),90):7.0f} | both {((rx==0)&(rz==0)).mean():.3f}")
stat("prev", c["vx"] - p["vx"], c["vz"] - p["vz"])
stat("lin", c["vx"] - (2*p["vx"] - pp["vx"]), c["vz"] - (2*p["vz"] - pp["vz"]))
stat("v2 (A=0)", Ax, Az)
stat("v2 + A(t-1)", Ax - Apx, Az - Apz)
# saturation-constrained z given exact x
sgn = np.where(Apz >= 0, 1, -1)
azs = sgn * np.floor(np.sqrt(np.maximum(acc.astype(float)**2 - Ax.astype(float)**2, 0)))
stat("vz|vx: sign(Az(t-1))*sqrt", Ax - Apx, Az - azs)
sat = np.hypot(Ax, Az) / acc > 0.999
print("saturated frac", sat[m].mean())
r = (Az - azs)[m & sat]
print("  saturated: z resid given x:", np.unique(r, return_counts=True)[0][:10], np.unique(r, return_counts=True)[1][:10], "exact", (r==0).mean(), "|r|<=1", (abs(r)<=1).mean())
# angular extrapolation needs A(t-2): t-3 state
ppp = {k: P.pp[k] for k in P.pp}
# build t-3 by shifting index: rows i-3 in the sorted S
from airpairs import FIELDS, field
i3 = np.maximum(P.i - 3, 0)
F3 = {k: field(P.S[i3], *FIELDS[k]) for k in ("vx", "vy", "vz", "h", "layer", "slot") if k in FIELDS}
# validity of t-3: same slot and consecutive tick -- recompute from air.npz order
z = np.load("air.npz"); o = np.lexsort((z["tick"], z["slot"])); tk = z["tick"][o]; sl = z["slot"][o]; cr = z["created"][o]
ok3 = (P.i >= 3) & (sl[i3] == P.slot) & (tk[i3] == P.tick - 3) & ~cr[np.maximum(P.i - 2, 0)] & ~cr[np.maximum(P.i - 1, 0)] & ~cr[P.i]
r3x, r3y, r3z, _ = known_part(P.typ, F3["h"], F3["vx"], F3["vy"], F3["vz"])
A2x, A2z = pp["vx"] - r3x, pp["vz"] - r3z
m3 = m & ok3 & (np.hypot(A2x, A2z) > 0) & (np.hypot(Apx, Apz) > 0)
# complex: A1 * (A1 / A2) normalised to |A1|
a1 = Apx + 1j * Apz; a2 = A2x + 1j * A2z
rot = (a1 / a2); rot = rot / np.abs(rot)
ap = a1 * rot
px, pz = np.trunc(ap.real), np.trunc(ap.imag)
rx, rz = (Ax - px)[m3], (Az - pz)[m3]
rx0, rz0 = (Ax - Apx)[m3], (Az - Apz)[m3]
print("on", m3.sum(), "rows: A(t-1)   x exact", (rx0==0).mean(), "p90", np.percentile(abs(rx0),90), "z exact", (rz0==0).mean(), "p90", np.percentile(abs(rz0),90))
print("            rotated x exact", (rx==0).mean(), "p90", np.percentile(abs(rx),90), "z exact", (rz==0).mean(), "p90", np.percentile(abs(rz),90))
pxr, pzr = np.rint(ap.real), np.rint(ap.imag)
rx, rz = (Ax - pxr)[m3], (Az - pzr)[m3]
print("    rotated(rint) x exact", (rx==0).mean(), "p90", np.percentile(abs(rx),90), "z exact", (rz==0).mean(), "p90", np.percentile(abs(rz),90))
