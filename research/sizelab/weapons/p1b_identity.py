"""p1b: the identity map by order-preserving match: the pool at T+1 is the survivors of T, in order,
then the records appended in step T. For each record of T+1, find it in T by key, monotone.
Reports how the +0x67 stamp relates, and how many records are new, how many survive, die."""
import sys, struct, collections
import numpy as np
from pool import iterations

def key(row):
    b = row.tobytes()
    return (struct.unpack_from("<I", b, 0)[0], struct.unpack_from("<I", b, 0x52)[0],
            struct.unpack_from("<I", b, 0x46)[0])   # weapon, attacker, death time

def idx67(row):
    return struct.unpack_from("<H", row.tobytes(), 0x67)[0]

prev = None
st = collections.Counter()
ex = []
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is not None:
        Q, qc = prev["pool"], prev["count"]
        qkeys = [key(Q[k]) for k in range(qc)]
        k = 0
        for j in range(c):
            kj = key(P[j])
            # monotone search forward
            m = k
            while m < qc and qkeys[m] != kj:
                m += 1
            if m < qc:
                st["survivor"] += 1
                st["died before it"] += m - k
                if idx67(P[j]) == m: st["survivor, 67 == old index"] += 1
                else:
                    st["survivor, 67 != old index"] += 1
                    if len(ex) < 10: ex.append((it["tick"], j, m, idx67(P[j]), qc, c))
                k = m + 1
            else:
                st["new"] += 1
                if idx67(P[j]) >= qc: st["new, 67 >= old count"] += 1
                else: st["new, 67 < old count"] += 1
        st["died at the end"] += qc - k
    prev = it
for kk, v in st.items():
    print(f"{kk:45s} {v}")
print(ex)
