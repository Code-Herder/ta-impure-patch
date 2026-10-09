"""p1: what the deltas are: a projectile moving in its slot, or another projectile sliding into the
slot (the compaction 0x49AE20). Checks +0x67 (the index before the compaction) against the
previous iteration's pool, and the identity key (weapon, attacker, spawn GameTime)."""
import sys, struct, collections
import numpy as np
from pool import iterations

def key(row):
    b = row.tobytes()
    weapon = struct.unpack_from("<I", b, 0)[0]
    spawn = struct.unpack_from("<I", b, 0x42)[0]
    attacker = struct.unpack_from("<I", b, 0x52)[0]
    return (weapon, attacker, spawn)

def idx67(row):
    return struct.unpack_from("<H", row.tobytes(), 0x67)[0]

prev = None
st = collections.Counter()
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is not None:
        Q, qc = prev["pool"], prev["count"]
        for j in range(c):
            k = idx67(P[j])
            st["live"] += 1
            if k < qc:
                if key(Q[k]) == key(P[j]):
                    st["67->same projectile"] += 1
                    if k != j: st["slid"] += 1
                    else: st["stayed"] += 1
                else:
                    st["67->other key (new or double compaction)"] += 1
            else:
                st["67 >= old count (new)"] += 1
        # stale tail
        for j in range(c, it["hw"]):
            if j < len(Q) and np.array_equal(P[j], Q[j]):
                st["tail unchanged"] += 1
            else:
                st["tail changed"] += 1
    prev = it
for k, v in st.items():
    print(f"{k:45s} {v}")
