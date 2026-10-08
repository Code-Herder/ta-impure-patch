"""capsearch.py -- L0: a replacement for codec2.search (the encoder's reference search), installed by
p10_l0.py. Same restriction as codec2.RESTRICT (rows of the same weapon, the old pool's live records
and the slot's own previous bytes), plus an optional cap: in each section of the candidate table
(out = rows decoded so far, Q = old pool, adv = old records advanced, ev = shot events) only the CAP
same-weapon rows nearest an anchor are compared. Anchors: out -> min(own, n_out-1) for a tail slot,
n_out-1 for a birth (the latest decoded row); Q and adv -> own for a tail slot, qc-1 (the newest old
record) for a birth; ev -> its last row. The decoder never searches, so the format is unchanged.
Counts: WORK = dword compares of the cost (rows x 27), FILTER = rows whose weapon dword is read,
BYSHOOTER: rows of the record's own shooter (+0x52) first, then by distance.
STATS = (section, distance rank of the chosen row within its section) of every uncapped choice.
With CAP = 0 the choice is codec2.search's under RESTRICT exactly (same candidates, same order)."""
import numpy as np
import collections
import codec2 as C2

CAP = 0
BYSHOOTER = False   # order each section by (other shooter, distance) instead of distance alone
WORK = [0]
FILTER = [0]
STATS = collections.Counter()

def search(T, row, n_out, qc, qhw, own=-1):
    n = len(T)
    FILTER[0] += n
    idx = np.arange(n)
    ok = T[:, 0] == row[0]
    stale = (idx >= n_out + qc) & (idx < n_out + qhw)
    ok &= ~stale
    if own >= 0:
        ok[n_out + own] = True
    b = [0, n_out, n_out + qhw, n_out + qhw + qc, n]          # section bounds
    if own >= 0:
        anc = [min(own, n_out - 1), n_out + own, n_out + qhw + min(own, qc - 1), n - 1]
    else:
        anc = [n_out - 1, n_out + qc - 1, n_out + qhw + qc - 1, n - 1]
    if not ok.any():
        ok[:] = True                                          # no row of its weapon: anything
    secs = []
    for s in range(4):
        c = np.nonzero(ok[b[s]:b[s + 1]])[0] + b[s]
        if len(c):
            key = np.abs(c - anc[s])
            if BYSHOOTER:     # the shooter is the u16 at +0x52: the high half of d20
                key = key + ((T[c, 20] >> 16) != (row[20] >> 16)) * (1 << 20)
            c = c[np.argsort(key, kind="stable")]
            if CAP:
                c = c[:CAP]
        secs.append(c)
    cand = np.sort(np.concatenate(secs))       # ascending: ties resolve as in codec2.search
    WORK[0] += len(cand) * 27
    r = int(cand[int(np.argmin(C2.cost(T[cand], row)))])
    if not CAP:
        for s in range(4):
            if b[s] <= r < b[s + 1]:
                rank = int(np.nonzero(secs[s] == r)[0][0])
                STATS[(s, rank)] += 1
    return r
