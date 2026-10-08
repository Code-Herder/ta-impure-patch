"""p18: seek. For each snapshot block: its projectile pool (RK_PROJ_HEAD and an RK_PROJ per record
below hw) equals the stream's pool at the snapshot's iteration; a fresh decoder holding only that
pool decodes the re-encoded stream from there to the next snapshot exactly."""
import sys, struct
import numpy as np
from pool import iterations, CAP
import codec as C
import codec4 as C4
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T

path = sys.argv[1]
bl, _ = T.blocks(path)
snaps = []
for kind, first, last, payload in bl:
    if kind != T.RB_SNAPSHOT: continue
    pool = np.zeros((CAP, 27), np.uint32); count = hw = None
    for rk, b in T.records(payload):
        if rk == T.RK_PROJ_HEAD: count, hw = struct.unpack_from("<II", b)
        elif rk == T.RK_PROJ:
            i = struct.unpack_from("<H", b)[0]
            pool[i] = np.frombuffer(b[4:4 + T.REC_PROJ_IMAGE], np.uint32)
    snaps.append((last, count, hw, pool[:hw].copy()))
print("snapshots at", [s[0] for s in snaps])
enc = C4.Codec4(True, True, False)
seekers = []      # (decoder, snapshot tick)
ok = bad = 0
lastshots = []
prev = None
si = 0
for it in iterations(path, want_recs=False):
    P, c, hw = it["pool"], it["count"], it["hw"]
    if it["snap_before"]:
        # the snapshot block sits between prev and it: its pool is prev's
        st, sc, shw, spool = snaps[si]; si += 1
        assert prev is not None and sc == prev["count"] and shw == prev["hw"] and \
            np.array_equal(spool, prev["pool"]), f"snapshot {st} differs from the stream"
        d = C4.Codec4(True, True, False)
        d.Q, d.qc, d.qhw = spool.copy(), sc, shw
        d.H = np.zeros((sc, 27), np.uint32); d.Hv = np.zeros(sc, bool)
        seekers.append([d, st])
    enc.set_shots(lastshots)
    ctl, rs, rn, rt, refs = enc.encode(P, c, hw, it["snap_before"])
    # every seeker decodes this iteration (the chosen modes: as p5 would choose, here 'best' fixed)
    for s in seekers:
        d = s[0]
        d.set_shots(lastshots)
        rsel = {col: (md, rs[col][md]) for col, md in
                ((col, "phys" if col in (1, 2, 3) else "lin" if col in (4, 5, 6, 8) else
                  "idx25" if col == 25 else "prev") for col in range(27))}
        Pd, cd, hwd = d.decode(ctl, rsel, rn, rt, refs, it["snap_before"] and s[1] != st)
        if cd == c and hwd == hw and np.array_equal(Pd, P): ok += 1
        else: bad += 1
    seekers = [s for s in seekers if it["tick"] - s[1] < 900]
    lastshots = it["shots"]
    prev = it
print(f"seek decodes: {ok} iterations exact, {bad} differing, from {len(snaps)} snapshots")
