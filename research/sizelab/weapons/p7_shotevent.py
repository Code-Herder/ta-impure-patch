"""p7: how a new projectile record relates to the RK_EV_SHOT of the step that made it: the event's
start point against the record's tail (+0x10) and its position less its velocity, its target point
against +0x28, its angles against +0x36/+0x38, its units against +0x4E/+0x52."""
import sys, struct, collections
import numpy as np
from pool import iterations, match
import codec as C

def rec_fields(row):
    b = row.tobytes()
    return dict(w=struct.unpack_from("<I", b, 0)[0],
                pos=struct.unpack_from("<3i", b, 4), tail=struct.unpack_from("<3i", b, 0x10),
                vel=struct.unpack_from("<3i", b, 0x1C), tgt=struct.unpack_from("<3i", b, 0x28),
                spin=struct.unpack_from("<H", b, 0x34)[0], head=struct.unpack_from("<H", b, 0x36)[0],
                pitch=struct.unpack_from("<H", b, 0x38)[0], target=struct.unpack_from("<I", b, 0x4E)[0],
                attacker=struct.unpack_from("<I", b, 0x52)[0], burst=struct.unpack_from("<H", b, 0x60)[0],
                piece=struct.unpack_from("<H", b, 0x62)[0])

def ev_fields(b):
    return dict(w=b[0x19] | ((b[0x23] >> 4) << 8), start=struct.unpack_from("<3i", b, 1),
                tgt=struct.unpack_from("<3i", b, 0x0D), head=struct.unpack_from("<H", b, 0x1B)[0],
                pitch=struct.unpack_from("<H", b, 0x1D)[0], target=struct.unpack_from("<H", b, 0x1F)[0],
                shooter=struct.unpack_from("<H", b, 0x21)[0], slot=b[0x23] & 0xF, flags=b[0x1A])

st = collections.Counter()
ex = []
prev = None
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is not None:
        Q, qc = prev["pool"], prev["count"]
        m = C.prefix_map(match(Q, qc, P, c), qc)
        evs = [ev_fields(b) for b in prev["shots"]]
        used = [False] * len(evs)
        for j in range(len(m), c):
            r = rec_fields(P[j])
            # a burst copy: a live template of the same weapon and attacker in the old pool
            cand = [k for k, e in enumerate(evs) if not used[k] and e["w"] + 1 == r["w"]
                    and e["shooter"] == r["attacker"]]
            if not cand:
                st["new without an event (burst copy or other)"] += 1
                continue
            k = cand[0]; used[k] = True; e = evs[k]
            st["new with its event"] += 1
            if r["burst"]: st["  ...a burst template"] += 1
            st["  tail == start"] += r["tail"] == e["start"]
            pmv = tuple(r["pos"][a] - r["vel"][a] for a in range(3))
            st["  pos - vel == start"] += pmv == e["start"]
            st["  pos == start"] += r["pos"] == e["start"]
            st["  tgt == event tgt"] += r["tgt"] == e["tgt"]
            st["  heading == event"] += r["head"] == e["head"]
            st["  pitch == event"] += r["pitch"] == e["pitch"]
            st["  target == event"] += r["target"] == e["target"]
            if len(ex) < 6 and not r["burst"]:
                ex.append((it["tick"], r["w"], r["pos"], r["vel"], r["tail"], e["start"], r["tgt"], e["tgt"], r["head"], r["pitch"], e["head"], e["pitch"]))
        st["events not matched"] += used.count(False)
    prev = it
for k, v in st.items():
    print(f"{k:45s} {v}")
for e in ex: print(e)
