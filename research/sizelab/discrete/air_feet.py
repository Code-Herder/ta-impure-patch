"""L0 (new): the airborne units (+0x110 & 3 == 2) per iteration, how many move their footprint
between two iterations' images, the footprint size, and the cells such a move changes."""
import collections, pickle
from common import OUT
from predict import rect
d = pickle.load(open(OUT / "units_pass.pkl", "rb"))
feet = d["feet"]
ticks = sorted(feet)
air = moved = cells = 0
sizes = collections.Counter()
per = []
for t in ticks[:-1]:
    a, b = feet[t], feet.get(t + 1, {})
    na = [s for s, f in a.items() if f[4] & 3 == 2]
    air += len(na)
    m = 0
    for s in na:
        sizes[(a[s][2], a[s][3])] += 1
        if s in b and b[s][:2] != a[s][:2]:
            m += 1
            cells += len(rect(a[s]) ^ rect(b[s]))
    moved += m
    per.append(len(na))
n = len(ticks) - 1
print(f"iterations {n}: airborne units a step mean {air / n:.1f} (max {max(per)}), footprint moves a step "
      f"{moved / n:.1f}, cells a move changes {cells / max(moved, 1):.2f}, cells a step {cells / n:.1f}")
print("footprint sizes (w, h):", sizes.most_common(5))
