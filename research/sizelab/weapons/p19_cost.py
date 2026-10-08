"""p19: the encoder's work per iteration, counted: the identity match's dword compares (each live
record against old records from the last match on, until one shares 12 dwords), and the reference
search's (each birth and changed tail slot against every candidate row, 27 dwords a row)."""
import sys
import numpy as np
from pool import iterations, match
import codec as C
prev = None
mt = []; rf = []
for it in iterations(sys.argv[1], want_recs=False):
    P, c, hw = it["pool"], it["count"], it["hw"]
    if prev is not None:
        Q, qc, qhw = prev["pool"], prev["count"], prev["hw"]
        mp = match(Q, qc, P, c)
        m = C.prefix_map(mp, qc)
        # compares: for each j, rows scanned = (matched index - search start + 1) or the window
        k = 0; n = 0
        for j in range(c):
            if mp[j] >= 0: n += int(mp[j]) - k + 1; k = int(mp[j]) + 1
            else: n += min(64, max(qc - k, 0))
        mt.append(n * 27)
        births = c - len(m)
        tail = sum(1 for j in range(c, hw) if not np.array_equal(P[j], Q[j] if j < qhw else np.zeros(27, np.uint32)))
        rows = c + qhw + qc + len(prev["shots"])
        rf.append((births + tail) * rows * 27)
    prev = it
mt = np.array(mt); rf = np.array(rf)
print(f"match: mean {mt.mean():.0f}, max {mt.max()} dword compares an iteration")
print(f"reference search: mean {rf.mean():.0f}, p99 {np.percentile(rf,99):.0f}, max {rf.max()} dword compares an iteration")
