import numpy as np
from airpairs import Pairs
from airseries import AIR_TYPES
import airdefs as D
from airstep import known_part

P = Pairs("air.npz")
c, p, pp = P.c, P.p, P.pp
m = ((c["layer"] & 3) == 2) & ((p["layer"] & 3) == 2)
v2x, v2y, v2z, over = known_part(P.typ, p["h"], p["vx"], p["vy"], p["vz"])
Ax, Az = c["vx"] - v2x, c["vz"] - v2z
# previous tick's A: need v(t-2) too
q2x, q2y, q2z, _ = known_part(P.typ, pp["h"], pp["vx"], pp["vy"], pp["vz"])
Apx, Apz = p["vx"] - q2x, p["vz"] - q2z
m2 = m & P.has_pp & ((pp["layer"] & 3) == 2)
U = 65536 / (2 * np.pi)
phi = np.arctan2(Ax, Az) * U
phip = np.arctan2(Apx, Apz) * U
def wrap(a): return (a + 32768) % 65536 - 32768
# heading direction of motion: v = -(sin h, cos h): angle of (vx,vz) is atan2(-sin,-cos) = h - 32768
nz = m2 & (np.hypot(Ax, Az) > 0) & (np.hypot(Apx, Apz) > 0)
for name, pred in [("phi(t-1)", phip), ("phi(t-1)+tr(t)", phip + c["tr"]), ("nose h(t)", c["h"] - 32768.0),
                   ("nose h(t-1)", p["h"] - 32768.0)]:
    d = np.abs(wrap(phi - pred))[nz]
    print(f"{name:16s} |dphi| p50 {np.percentile(d,50):8.1f} p90 {np.percentile(d,90):8.1f} p99 {np.percentile(d,99):8.1f}  (65536 = 360 deg)")
# component residual if direction known from phi(t-1)+tr: predicted Ax
acc = D.per_type(P.typ, "acc")
for name, pred in [("phi(t-1)", phip), ("phi(t-1)+tr(t)", phip + c["tr"])]:
    th = pred / U
    px = np.trunc(acc * np.sin(th)); pz = np.trunc(acc * np.cos(th))
    rx = (Ax - px)[nz]; rz = (Az - pz)[nz]
    print(f"{name:16s} Ax resid p50 {np.percentile(abs(rx),50):.0f} p90 {np.percentile(abs(rx),90):.0f} exact {(rx==0).mean():.3f}")
# vertical
sp_prev = p["sp"]
cc = np.where((sp_prev & ~3) < 0x40000, 0x10000, sp_prev >> 2)
vy = c["vy"]
print("vy: saturated +c", ((vy == cc) & m).sum(), " -c", ((vy == -cc) & m).sum(), " zero", ((vy == 0) & m).sum(), " other", (m & (vy != cc) & (vy != -cc) & (vy != 0)).sum(), "of", m.sum())
print("vy unchanged", ((vy == p["vy"]) & m).sum())
print("offmap/skip?", "bucket==? unknown")
