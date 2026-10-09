"""p3b: the transitions where an unmatched record sits before a matched one: what are they?"""
import sys, collections
import numpy as np
from pool import iterations, match
prev = None; n = 0; st = collections.Counter()
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is not None:
        Q, qc = prev["pool"], prev["count"]
        mp = match(Q, qc, P, c)
        s = np.nonzero(mp >= 0)[0]
        if len(s) and s[-1] + 1 != len(s):
            st["transitions"] += 1
            holes = [j for j in range(s[-1]) if mp[j] < 0]
            st["holes"] += len(holes)
            if n < 6:
                n += 1
                j = holes[0]
                lo = mp[j - 1] if j > 0 else -1
                hi = mp[j + 1] if j + 1 < c else qc
                print("tick", it["tick"], "slot", j, "between old", lo, hi, "qc", qc, "c", c)
                for k in range(lo + 1, min(hi, lo + 4)):
                    eq = (Q[k] == P[j]).sum()
                    print("   old", k, "equal dwords", eq, "diff cols", np.nonzero(Q[k] != P[j])[0].tolist())
                print("   new w", P[j, 0], "old w", [int(Q[k, 0]) for k in range(lo + 1, min(hi, lo + 4))])
    prev = it
print(st)
