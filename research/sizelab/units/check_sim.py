import time, numpy as np, sim
z = sim.load("units.npz")
snaps = set(int(t) for t in z["snap_ticks"])
t0 = time.time(); n = 0
for tk in sim.ticks(z):
    n += 1
    if tk.gametime in snaps:
        sl = z[f"snap_{tk.gametime}_slots"]; im = z[f"snap_{tk.gametime}_imgs"]
        ok = set(np.flatnonzero(tk.live)) == set(sl.tolist()) and np.array_equal(tk.S[sl], im)
        print(tk.gametime, "live", tk.live.sum(), "snapshot units", len(sl), "EQUAL" if ok else "DIFFER")
print(n, "ticks", time.time() - t0, "s")
