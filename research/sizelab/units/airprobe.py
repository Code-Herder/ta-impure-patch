"""airprobe.py -- aircraft (layer 2) unit-iterations: candidate relations for the mover velocity
+0x08..+0x10 and the new lanes +0x14..+0x1C. Exploration only; frame2 is not changed."""
import numpy as np, sim, frame2
from sintab import T as SIN
SIN = SIN.astype(np.int64)
z = sim.load("units.npz")
f = lambda S, o, n=4, s=True: sim.field(S, o, n, s)
M = 0x118
prev = None; prevlive = np.zeros(sim.SLOTS, bool)
acc = {}
def add(k, m):
    a = acc.setdefault(k, [0, 0]); a[0] += int(m.size); a[1] += int(m.sum())
for tk in sim.ticks(z):
    S = tk.S
    cur = {k: f(S, o, n, sg) for k, (o, n, sg) in {
        "vx": (M+8,4,1), "vy": (M+0xC,4,1), "vz": (M+0x10,4,1), "ax": (M+0x14,4,1), "ay": (M+0x18,4,1),
        "az": (M+0x1C,4,1), "sp": (M+0x20,4,1), "tr": (M+0x24,2,1), "h": (0x66,2,0), "p": (0x64,2,0),
        "lay": (M+0x2E,1,0), "x": (0x6A,4,1), "y": (0x6E,4,1), "z": (0x72,4,1)}.items()}
    if prev is not None:
        ok = tk.live & prevlive & ((cur["lay"] & 3) == 2) & ((prev["lay"] & 3) == 2)
        if tk.created: ok[tk.created] = False
        if ok.any():
            c = {k: v[ok] for k, v in cur.items()}; p = {k: v[ok] for k, v in prev.items()}
            for a in "xyz":
                add(f"v{a}(t) == a{a}(t)", c["v"+a] == c["a"+a])
                add(f"v{a}(t) == v{a}(t-1) + a{a}(t)", c["v"+a] == p["v"+a] + c["a"+a])
                add(f"v{a}(t) == v{a}(t-1) + a{a}(t-1)", c["v"+a] == p["v"+a] + p["a"+a])
                add(f"v{a} unchanged", c["v"+a] == p["v"+a])
                add(f"a{a} unchanged", c["a"+a] == p["a"+a])
            # ground rule shape with the air speed
            for hn in ("h",):
                h, s = c[hn], c["sp"]
                vx = -((SIN[((h + 0x20) >> 7) & 511] * s + 0x1000) >> 13)
                vz = -((SIN[((h + 0x4020) >> 7) & 511] * s + 0x1000) >> 13)
                add("vx == -sin(h)*speed", c["vx"] == vx); add("vz == -cos(h)*speed", c["vz"] == vz)
            mag = np.sqrt(c["vx"].astype(float)**2 + c["vz"].astype(float)**2)
            add("|v_xz| within 1 of speed", np.abs(mag - c["sp"]) <= 1.5)
    prev = cur; prevlive = tk.live.copy()
for k, (n, m) in acc.items():
    print(f"{k:40s} {m:9d} / {n:9d} = {m/max(n,1):.2%}")
