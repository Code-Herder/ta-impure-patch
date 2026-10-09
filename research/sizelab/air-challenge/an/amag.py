import sys
import numpy as np
from airpairs import Pairs
import frame3 as f3, airdefs as D
P = Pairs(sys.argv[1])
c, p, pp = P.c, P.p, P.pp
tt = f3.TypeTable(D.DEF, D.GRAVITY, D.SEA)
kx, ky, kz = f3.known_part(tt, P.typ, p["h"], p["vx"], p["vy"], p["vz"])
ax, az = c["vx"] - kx, c["vz"] - kz
m = ((c["layer"] & 3) == 2) & ((p["layer"] & 3) == 2)
a = np.maximum(np.abs(ax[m]), np.abs(az[m]))
print("PD term |A| max over airborne pairs:", a.max(), " >2^16:", (a > 1 << 16).sum(), " >2^15:", (a > 1 << 15).sum(), "of", m.sum())
num = np.abs(ax[m]).astype(np.float64) * (2 * a.astype(np.float64) ** 2)
print("rot numerator bound |x1|*2*max^2 max:", num.max(), " log2", np.log2(num.max()))
# gravity sensitivity of the bank rule
for G in (8150, 8154, 8155, 8156, 8160):
    q = (G << 16) // 0xCCD
    b = f3.bank_angle(c["ax"][m], c["az"][m], c["h"][m], tt.bank[P.typ[m]], q) & 0xFFFF
    print("gravity", G, "q", q, "bank exact", int((b == (c["bank"][m] & 0xFFFF)).sum()))
