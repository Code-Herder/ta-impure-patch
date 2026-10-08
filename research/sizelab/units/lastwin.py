"""lastwin.py -- per unit-lane predictor switching by last tick's winner, for x and z."""
import numpy as np
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
g = lambda k: np.asarray(L(k)).astype(np.int64)
live = np.asarray(L("live"))
ok3 = live[3:] & live[2:-1] & live[1:-2] & live[:-3] & (np.asarray(L("mover"))[3:] != 0)
x, z, vx, vz, ml = g("x"), g("z"), g("vx"), g("vz"), g("mlayer")
for nm, a, v in (("x", x, vx), ("z", z, vz)):
    # predictions at time t (index t), for t >= 2
    def preds(t0, t1, t2, vt):
        return {"rule": t1 + vt, "lin": 2 * t1 - t2, "prev": t1}
    P = preds(a[2:], a[1:-1], a[:-2], v[2:])        # predictions for a[2:]
    hit = {k: (p == a[2:]) for k, p in P.items()}
    # winner at tick t-1 (index into the [2:] arrays shifted by one)
    order = ["rule", "lin", "prev"]
    prevwin = np.full(hit["rule"][:-1].shape, 2, np.int8)          # default prev
    for i, k in reversed(list(enumerate(order))):
        prevwin = np.where(hit[k][:-1], i, prevwin)
    # predict tick t (>=3) with last winner
    choice = prevwin
    pr = np.choose(choice, [P["rule"][1:], P["lin"][1:], P["prev"][1:]])
    miss = (pr != a[3:]) & ok3
    print(nm, "last-winner misses", miss.sum(), " fixed: rule", ((P["rule"][1:] != a[3:]) & ok3).sum(),
          "lin", ((P["lin"][1:] != a[3:]) & ok3).sum(), "prev", ((P["prev"][1:] != a[3:]) & ok3).sum(),
          " oracle", (ok3 & ~(hit["rule"][1:] | hit["lin"][1:] | hit["prev"][1:])).sum())
