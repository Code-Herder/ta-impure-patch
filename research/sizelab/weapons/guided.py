"""guided.py -- L0: does a projectile's velocity follow the guided rule (DISASSEMBLED 0x49BA74..0x49BABE)
  vy = S(pitch +0x38, speed +0x3A); h = C(pitch, speed); vx = -S(heading +0x36, h); vz = -C(heading, h)
with S = 0x4B70EF, C = 0x4B7123 (the sine table 0x509F00, imul, +0x1000, shrd 13), evaluated on the
record's own (new) angles and speed? Per weapon: survivor transitions, those whose velocity changed,
rule exact on the new record, and exact among the changed ones."""
import sys, os, collections
import numpy as np
sys.path.insert(1, str(__import__("pathlib").Path(__file__).resolve().parents[1] / "weapons"))
os.environ.setdefault("TA_PRISTINE", "pristine/TotalA.exe.pristine")
sys.path.insert(1, str(__import__("pathlib").Path(__file__).resolve().parents[1] / "units"))
from sintab import T as SIN
from pool import iterations, match
import codec as C
SIN = SIN.astype(np.int64)

def _mul(e, s):
    v = (SIN[e] * s + 0x1000) >> 13          # 64-bit arithmetic shift; shrd keeps the low 32 bits
    return v & 0xFFFFFFFF
def S(a, s):   # a: uint16 angle, s: int32
    a = a.astype(np.int64); a = np.where(a >= 32768, a - 65536, a)
    e = (((a + 0x20) & 0xFFFFFFFF) >> 6 & 0x3FE) >> 1
    return _mul(e, s)
def Cc(a, s):
    e = (((a.astype(np.int64) + 0x4020) & 0xFFFFFFFF) >> 6 & 0x3FE) >> 1
    return _mul(e, s)
def s32(x): x = x.astype(np.int64); return np.where(x >= 2**31, x - 2**32, x)

def rule(R):
    """R: uint32 rows [n, 27] -> predicted (vx, vy, vz) as uint32 [n, 3]"""
    b = R.astype("<u4").view(np.uint8).reshape(len(R), 108)
    head = b[:, 0x36].astype(np.int64) | (b[:, 0x37].astype(np.int64) << 8)
    pitch = b[:, 0x38].astype(np.int64) | (b[:, 0x39].astype(np.int64) << 8)
    sp = s32(b[:, 0x3A:0x3E].copy().view("<u4").ravel())
    vy = S(pitch, sp); h = s32(Cc(pitch, sp))
    vx = (-s32(S(head, h))) & 0xFFFFFFFF; vz = (-s32(Cc(head, h))) & 0xFFFFFFFF
    return np.stack([vx, vy, vz], 1).astype(np.uint32)

st = collections.defaultdict(collections.Counter)
prev = None
for it in iterations(sys.argv[1], want_recs=False):
    P, c = it["pool"], it["count"]
    if prev is not None and c:
        Q, qc = prev["pool"], prev["count"]
        m = C.prefix_map(match(Q, qc, P, c), qc)
        ns = len(m)
        if ns:
            new = P[:ns]; old = Q[m]
            pr = rule(new)
            ex = (pr == new[:, 7:10]).all(1)
            chg = (new[:, 7:10] != old[:, 7:10]).any(1)
            hchg = (new[:, [7, 9]] != old[:, [7, 9]]).any(1)
            for w in np.unique(new[:, 0]):
                k = new[:, 0] == w
                s = st[int(w)]
                s["trans"] += int(k.sum()); s["vel chg"] += int((k & chg).sum())
                s["horiz chg"] += int((k & hchg).sum())
                s["rule exact"] += int((k & ex).sum()); s["rule exact, chg"] += int((k & ex & chg).sum())
                s["rule exact, horiz chg"] += int((k & ex & hchg).sum())
        # births too: does a new record's velocity follow the rule at birth?
        if c > ns:
            pr = rule(P[ns:c]); ex = (pr == P[ns:c, 7:10]).all(1)
            for w in np.unique(P[ns:c, 0]):
                k = P[ns:c, 0] == w
                st[int(w)]["births"] += int(k.sum()); st[int(w)]["births rule exact"] += int((k & ex).sum())
    prev = it
keys = ["trans", "vel chg", "horiz chg", "rule exact", "rule exact, chg", "rule exact, horiz chg", "births", "births rule exact"]
print(f"{'weapon':>6s} " + " ".join(f"{k[:18]:>18s}" for k in keys))
tot = collections.Counter()
for w, s in sorted(st.items(), key=lambda x: -x[1]["trans"]):
    print(f"{w:6d} " + " ".join(f"{s[k]:18d}" for k in keys))
    tot.update(s)
print(f"{'all':>6s} " + " ".join(f"{tot[k]:18d}" for k in keys))
