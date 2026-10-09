"""codec2.py -- codec.py with a better reference for the records that are not survivors.

A new record (appended in the step) and a changed tail slot (the compaction's leftovers, which
hold the pre-compaction record of that index) are coded against the best reference the encoder
finds among the candidates both sides hold: the new pool's records decoded so far, the old pool
whole (stale tail included), and every old live record advanced by the survivors' predictor.
The reference is signalled as its candidate index less a default guess (zigzag): for a tail slot
the record the compaction would have left there if no appended record died in the step, for a new
record the previous new record. Generic: no field is named; the stamp +0x67 rides the 'idx'
predictor of codec.py for survivors and the advanced old records."""
import numpy as np
import codec as C
from codec import ND, U, ORDER, predict, prefix_map, match

def leblen(z):
    z = z.astype(np.uint64)
    return 1 + (z >= 2**7) + (z >= 2**14) + (z >= 2**21) + (z >= 2**28)

RESTRICT = False      # the encoder searches only rows of the same weapon, never the stale tail
WORK = [0]            # dword compares the search made (rows searched x 27)

def search(T, row, n_out, qc, qhw, own=-1):
    """the encoder's choice of reference: the cheapest row of T. With RESTRICT, only rows whose
    first dword (the weapon) is the row's, and of the old pool only its live records (and the
    slot's own previous bytes, `own`): the decoder never searches, so this is the encoder's
    business alone and the format does not change."""
    if RESTRICT:
        idx = np.arange(len(T))
        ok = T[:, 0] == row[0]
        stale = (idx >= n_out + qc) & (idx < n_out + qhw)
        ok &= ~stale
        if own >= 0:
            ok[n_out + own] = True
        cand = np.nonzero(ok)[0]
        if not len(cand):
            cand = idx
        WORK[0] += len(cand) * 27
        c = cost(T[cand], row)
        return int(cand[int(np.argmin(c))])
    WORK[0] += len(T) * 27
    return int(np.argmin(cost(T, row)))

def cost(cands, row):
    d = (row.astype(np.int64)[None, :] - cands.astype(np.int64)) % 2**32
    z = C.zz(d.astype(U).ravel()).reshape(d.shape)
    return leblen(z).sum(axis=1)

class Codec2(C.Codec):
    def advanced(self, Q, qc):
        """every old live record as the survivors' predictor would carry it one tick on, its
        velocity predicted too (a dead record's new velocity is not known)"""
        if qc == 0:
            return np.zeros((0, ND), U)
        m = np.arange(qc)
        prev = Q[:qc]; prev2 = self.H[:qc] if len(self.H) >= qc else np.zeros((qc, ND), U)
        valid = self.Hv[:qc] if len(self.Hv) >= qc else np.zeros(qc, bool)
        cur = np.zeros((qc, ND), U)
        for col in ORDER:
            mode = self.modes.get(col, "prev")
            if mode == "phys":
                pass
            cur[:, col] = predict(col, mode if col not in (7, 8, 9) else "lin", prev, prev2, valid,
                                  cur, m)
        return cur

    def encode(self, P, c, hw, snap, m=None):
        if snap:      # a seek starts here: nothing from before the snapshot but the pool
            self.Hv = np.zeros(len(self.Hv), bool)
            self.lastnew = {}
        Q, qc, qhw = self.Q, self.qc, self.qhw
        if m is None:
            m = prefix_map(match(Q, qc, P, c), qc)
        ns = len(m)
        dead = np.setdiff1d(np.arange(qc), m)
        ctl = [c, hw, len(dead)]
        last = -1
        for k in dead:
            ctl.append(int(k) - last - 1); last = int(k)
        tail = [] if getattr(self, "notail", False) else \
            [j for j in range(c, hw) if not np.array_equal(P[j], Q[j] if j < qhw else np.zeros(ND, U))]
        ctl.append(len(tail)); last = c - 1
        for j in tail:
            ctl.append(j - last - 1); last = j
        res = self._code(P, c, m, tail, encode=True)
        self._advance(P, c, hw, m)
        return ctl, res[0], res[1], res[2], res[3]

    def _code(self, P, c, m, tail, encode, res=None):
        Q, qc, qhw = self.Q, self.qc, self.qhw
        ns = len(m)
        prev = Q[m] if ns else np.zeros((0, ND), U)
        prev2 = self.H[m] if ns else np.zeros((0, ND), U)
        valid = self.Hv[m] if ns else np.zeros(0, bool)
        rs = {}
        cur = P[:ns].copy() if encode else np.zeros((ns, ND), U)
        for col in ORDER:
            pred = predict(col, self.modes.get(col, "prev"), prev, prev2, valid, cur, m)
            if encode:
                rs[col] = (cur[:, col] - pred).astype(U)
            else:
                cur[:, col] = (pred + res[0][col]).astype(U)
        adv = self.advanced(Q, qc)
        # the old live records' new index, for the tail's default guess
        newidx = np.full(qc, -1, np.int64)
        newidx[m] = np.arange(ns)
        nnew = c - ns
        out = np.zeros((c, ND), U)
        out[:ns] = cur
        refs = []
        rn = {col: np.zeros(nnew, U) for col in range(ND)}
        # candidates table: [out[0:c) as decoded so far | Q[0:qhw) | adv[0:qc)]
        def table(n_out):
            return np.concatenate([out[:n_out], Q[:qhw], adv])
        for i in range(nnew):
            T = table(ns + i)
            guess = ns + i - 1 if i > 0 else max(ns - 1, 0)
            if encode:
                row = P[ns + i]
                r = int(np.argmin(cost(T, row))) if len(T) else -1
                if r < 0:
                    ref = np.zeros(ND, U); refs.append(0)
                else:
                    ref = T[r]; refs.append(int(C.zz(np.array([(r - guess) % 2**32], U))[0]) + 1)
                for col in range(ND):
                    rn[col][i] = U((int(row[col]) - int(ref[col])) & 0xFFFFFFFF)
                out[ns + i] = row
            else:
                z = int(res[3][len(refs)]); refs.append(z)
                if z == 0:
                    ref = np.zeros(ND, U)
                else:
                    r = (guess + int(C.unzz(np.array([z - 1], U))[0])) % 2**32
                    ref = T[r]
                out[ns + i] = (ref + np.array([res[1][col][i] for col in range(ND)], U)).astype(U)
        rt = {col: np.zeros(len(tail), U) for col in range(ND)}
        T = table(c)
        trows = np.zeros((len(tail), ND), U)
        for t, j in enumerate(tail):
            if j < qc:
                guess = int(newidx[j]) if newidx[j] >= 0 else c + qhw + j
            else:
                k = ns + (j - qc)
                guess = k if k < c else j
            if encode:
                row = P[j]
                r = int(np.argmin(cost(T, row)))
                refs.append(int(C.zz(np.array([(r - guess) % 2**32], U))[0]) + 1)
                for col in range(ND):
                    rt[col][t] = U((int(row[col]) - int(T[r][col])) & 0xFFFFFFFF)
            else:
                z = int(res[3][len(refs)]); refs.append(z)
                r = (guess + int(C.unzz(np.array([z - 1], U))[0])) % 2**32
                trows[t] = (T[r] + np.array([res[2][col][t] for col in range(ND)], U)).astype(U)
        if encode:
            return rs, rn, rt, np.array(refs, U)
        return out, trows

    def decode(self, ctl, rs, rn, rt, refs, snap):
        if snap:      # a seek starts here: nothing from before the snapshot but the pool
            self.Hv = np.zeros(len(self.Hv), bool)
            self.lastnew = {}
        Q, qc, qhw = self.Q, self.qc, self.qhw
        it = iter(ctl)
        c, hw, nd = next(it), next(it), next(it)
        dead = []; last = -1
        for _ in range(nd):
            last = last + 1 + next(it); dead.append(last)
        m = np.setdiff1d(np.arange(qc), np.array(dead, np.int64))
        nt = next(it); tail = []; last = c - 1
        for _ in range(nt):
            last = last + 1 + next(it); tail.append(last)
        out, trows = self._code(None, c, m, tail, encode=False, res=(rs, rn, rt, refs))
        P = np.zeros((hw, ND), U)
        n = min(hw, qhw)
        P[:n] = Q[:n]
        P[:c] = out
        if tail:
            P[tail] = trows
        self._advance(P, c, hw, m)
        return P, c, hw
