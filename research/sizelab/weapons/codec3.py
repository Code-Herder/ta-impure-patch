"""codec3.py -- codec2.py with the survivors' predictor chosen per column per block by the encoder
(signalled in the block: a mode index per column), and the +0x67 stamp of a new record predicted
as its pre-compaction index (the old count plus its rank among the new) when STAMP is on.

Candidate predictors per column: prev, lin; for the position-like columns (1..6) also phys; for
d25/d26 (+0x67) also the stamp (idx). Generic but for the two engine facts named: positions move
by their velocity (phys) and the compaction stamps +0x67 (idx)."""
import numpy as np
import codec as C
import codec2 as C2
from codec import ND, U, ORDER, prefix_map, match

def predict3(col, mode, prev, prev2, valid, cur, m):
    if mode == "idx25":   # low 24 bits linear, the top byte the stamp's low byte
        lin = C.predict(col, "lin", prev, prev2, valid, cur, m).astype(np.int64)
        return ((lin & 0x00FFFFFF) | ((m & 0xFF) << 24)).astype(U)
    if mode == "idx25p":
        return C.predict(col, "idx25", prev, prev2, valid, cur, m)
    return C.predict(col, mode, prev, prev2, valid, cur, m)

def modes_for(col, stamp=True):
    ms = ["prev", "lin"]
    if 1 <= col <= 6: ms.append("phys")
    if stamp and col == 25: ms += ["idx25", "idx25p"]
    if stamp and col == 26: ms.append("idx26")
    return ms

FIXED_ADV = {**{d: "lin" for d in range(27)}, 1: "phys", 2: "phys", 3: "phys"}

class Codec3(C2.Codec2):
    def __init__(self, stamp=True):
        super().__init__(FIXED_ADV)
        self.stamp = stamp

    def advanced(self, Q, qc):
        if qc == 0:
            return np.zeros((0, ND), U)
        m = np.arange(qc)
        prev = Q[:qc]; prev2 = self.H[:qc]; valid = self.Hv[:qc]
        cur = np.zeros((qc, ND), U)
        for col in ORDER:
            mode = FIXED_ADV[col]
            if self.stamp and col == 25: mode = "idx25"
            if self.stamp and col == 26: mode = "idx26"
            cur[:, col] = predict3(col, mode, prev, prev2, valid, cur, m)
        return cur

    def _code(self, P, c, m, tail, encode, res=None):
        """encode: residuals of the survivors under every candidate mode (dict col -> mode ->
        array); decode: res[0] is dict col -> (mode, residuals)"""
        Q, qc, qhw = self.Q, self.qc, self.qhw
        ns = len(m)
        prev = Q[m] if ns else np.zeros((0, ND), U)
        prev2 = self.H[m] if ns else np.zeros((0, ND), U)
        valid = self.Hv[m] if ns else np.zeros(0, bool)
        rs = {}
        cur = P[:ns].copy() if encode else np.zeros((ns, ND), U)
        for col in ORDER:
            if encode:
                rs[col] = {md: (cur[:, col] - predict3(col, md, prev, prev2, valid, cur, m)).astype(U)
                           for md in modes_for(col, self.stamp)}
            else:
                md, r = res[0][col]
                cur[:, col] = (predict3(col, md, prev, prev2, valid, cur, m) + r).astype(U)
        adv = self.advanced(Q, qc)
        newidx = np.full(qc, -1, np.int64)
        newidx[m] = np.arange(ns)
        nnew = c - ns
        out = np.zeros((c, ND), U)
        out[:ns] = cur
        refs = []
        rn = {col: np.zeros(nnew, U) for col in range(ND)}
        def table(n_out):
            return np.concatenate([out[:n_out], Q[:qhw], adv])
        def stamped(ref, i):
            if not self.stamp:
                return ref
            ref = ref.copy(); s = qc + i
            ref[25] = U((int(ref[25]) & 0x00FFFFFF) | ((s & 0xFF) << 24))
            ref[26] = U((int(ref[26]) & 0xFFFFFF00) | ((s >> 8) & 0xFF))
            return ref
        for i in range(nnew):
            T = table(ns + i)
            guess = ns + i - 1 if i > 0 else max(ns - 1, 0)
            if encode:
                row = P[ns + i]
                if len(T):
                    Ts = T.copy()
                    if self.stamp:
                        s = qc + i
                        Ts[:, 25] = (Ts[:, 25].astype(np.int64) & 0x00FFFFFF) | ((s & 0xFF) << 24)
                        Ts[:, 26] = (Ts[:, 26].astype(np.int64) & 0xFFFFFF00) | ((s >> 8) & 0xFF)
                    r = int(np.argmin(C2.cost(Ts, row)))
                    ref = Ts[r]; refs.append(int(C.zz(np.array([(r - guess) % 2**32], U))[0]) + 1)
                else:
                    ref = np.zeros(ND, U); refs.append(0)
                for col in range(ND):
                    rn[col][i] = U((int(row[col]) - int(ref[col])) & 0xFFFFFFFF)
                out[ns + i] = row
            else:
                z = int(res[3][len(refs)]); refs.append(z)
                if z == 0:
                    ref = np.zeros(ND, U)
                else:
                    r = (guess + int(C.unzz(np.array([z - 1], U))[0])) % 2**32
                    ref = stamped(T[r], i)
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
                r = C2.search(T, row, c, qc, qhw, j if j < qhw else -1)
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
