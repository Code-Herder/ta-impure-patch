"""tailmiss.py -- L0: the births whose tail is not their shot event's fire point (p7's pairing: first
unused event of the same weapon and shooter). For each: is there ANY event of that step, same weapon and
shooter, whose fire point equals the tail (a pairing artefact), and the tail - start delta otherwise."""
import sys, collections, struct
sys.path.insert(1, str(__import__("pathlib").Path(__file__).resolve().parents[1] / "weapons"))
from pool import iterations, match
import codec as C

# rec_fields and ev_fields copied from p7_shotevent.py
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


st = collections.Counter(); deltas = collections.Counter(); byw = collections.Counter()
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
            cand = [k for k, e in enumerate(evs) if not used[k] and e["w"] + 1 == r["w"] and e["shooter"] == r["attacker"]]
            if not cand: continue
            k = cand[0]; used[k] = True; e = evs[k]
            st["paired"] += 1
            if r["tail"] == e["start"]: continue
            st["tail != start"] += 1; byw[r["w"]] += 1
            if any(evs[q]["start"] == r["tail"] for q in range(len(evs)) if evs[q]["w"] + 1 == r["w"] and evs[q]["shooter"] == r["attacker"]):
                st["  another event of the shooter has it (pairing)"] += 1
            if len(cand) > 1: st["  shooter fired several of the weapon this step"] += 1
            d = tuple(r["tail"][a] - e["start"][a] for a in range(3))
            st["  tail - start == pos - tail (one tick late)" ] += d == tuple(r["pos"][a] - r["tail"][a] for a in range(3))
            st["  tail - start == vel"] += d == r["vel"]
            deltas[tuple(int(x).bit_length() if x >= 0 else -int(-x).bit_length() for x in d)] += 1
    prev = it
for k, v in st.items(): print(f"{k:55s} {v}")
print("by weapon", byw.most_common(8))
print("delta magnitude (bits, signed) per axis", deltas.most_common(8))
