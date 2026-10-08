"""codec.py -- a lossless re-encoding of the projectile pool's stream, by projectile identity.

The pool at iteration T+1 is the survivors of T in their order (the compaction 0x49AE20 slides
them down, it never reorders), then the records appended in step T (every append is at the
count), then the stale tail [count, hw) the compaction leaves. The transition is coded as:
  ctl:   count, hw, the dead old records (gaps), the tail slots that changed (gaps)
  surv:  each surviving record's 27 dwords as residuals against a prediction from the same
         projectile's previous states (column by column)
  new:   each appended record's dwords against a reference (the last new record of its weapon)
  tail:  each changed tail slot against the slot's previous bytes
Predictors per column: 'prev', 'lin' (2 prev - prev2), 'phys' (position: prev + new velocity +
last tick's drift), 'idx' (+0x67 stamped with the old index, then 'prev' for the other bytes).
History (prev2) restarts after each snapshot, so a seek from a snapshot decodes forward."""
import struct
import numpy as np
from pool import match

ND = 27
U = np.uint32

def zz(v):            # residual (uint32 array) -> zigzag uint32
    s = v.astype(np.int64)
    s = np.where(s >= 2**31, s - 2**32, s)
    return ((s << 1) ^ (s >> 63)).astype(np.uint64).astype(np.uint32)

def unzz(z):
    z = z.astype(np.int64)
    s = (z >> 1) ^ -(z & 1)
    return (s % 2**32).astype(np.uint32)

POS = {1: 7, 2: 8, 3: 9, 4: 7, 5: 8, 6: 9}    # position-like dword -> its velocity dword

def predict(col, mode, prev, prev2, valid, cur_vel, m):
    """the prediction of column `col` for survivors: prev/prev2 the old states (rows), valid the
    rows whose prev2 exists, cur_vel the new velocity rows (decoded first), m the old indices"""
    p = prev[:, col].astype(np.int64)
    if mode == "prev":
        out = p
    elif mode == "lin":
        out = np.where(valid, 2 * p - prev2[:, col].astype(np.int64), p)
    elif mode == "phys":
        v = POS[col]
        drift = p - prev2[:, col].astype(np.int64) - prev[:, v].astype(np.int64)
        out = np.where(valid, p + cur_vel[:, v].astype(np.int64) + drift,
                       p + cur_vel[:, v].astype(np.int64))
    elif mode == "idx25":
        out = (p & 0x00FFFFFF) | ((m & 0xFF) << 24)
    elif mode == "idx26":
        out = (p & ~0xFF & 0xFFFFFFFF) | ((m >> 8) & 0xFF)
    else:
        raise ValueError(mode)
    return (out % 2**32).astype(U)

def prefix_map(mp, qc):
    """The survivors as a prefix of the new pool, each paired with an old index in increasing
    order (the compaction keeps the order and appends go after). A record the matcher left
    unpaired before a paired one is paired with an unused old index between its neighbours' when
    there is one; otherwise the survivors end there and the rest are coded as new. Any pairing is
    lossless; this only decides what the residuals are taken against."""
    out = []
    last = -1
    for j in range(len(mp)):
        if mp[j] >= 0:
            out.append(int(mp[j])); last = int(mp[j]); continue
        later = [int(x) for x in mp[j + 1:] if x >= 0]
        if not later:
            break
        if later[0] > last + 1:
            last += 1; out.append(last)
        else:
            break
    return np.array(out, np.int64)

ORDER = [7, 8, 9] + [d for d in range(ND) if d not in (7, 8, 9)]   # velocity first

class Codec:
    def __init__(self, modes):
        self.modes = modes          # column -> predictor for survivors
        self.reset_all()

    def reset_all(self):
        self.Q = np.zeros((0, ND), U); self.qc = 0; self.qhw = 0
        self.H = np.zeros((0, ND), U); self.Hv = np.zeros(0, bool)
        self.lastnew = {}

    # ---- the encoder: returns (ctl ints, {col: residuals surv}, {col: residuals new}, {col: tail})
    def encode(self, P, c, hw, snap):
        if snap:
            self.Hv = np.zeros(len(self.Hv), bool)
        Q, qc, qhw = self.Q, self.qc, self.qhw
        m = prefix_map(match(Q, qc, P, c), qc)
        ns = len(m)
        dead = np.setdiff1d(np.arange(qc), m)
        ctl = [c, hw, len(dead)]
        last = -1
        for k in dead:
            ctl.append(int(k) - last - 1); last = int(k)
        # tail slots that changed
        tail = []
        for j in range(c, hw):
            old = Q[j] if j < qhw else np.zeros(ND, U)
            if not np.array_equal(P[j], old):
                tail.append(j)
        ctl.append(len(tail)); last = c - 1
        for j in tail:
            ctl.append(j - last - 1); last = j
        rs, rn, rt = self._residuals(P, c, m, tail, Q, qhw, encode=True)
        self._advance(P, c, hw, m)
        return ctl, rs, rn, rt

    def _residuals(self, P, c, m, tail, Q, qhw, encode, res=None):
        ns = len(m)
        prev = Q[m] if ns else np.zeros((0, ND), U)
        prev2 = self.H[m] if ns else np.zeros((0, ND), U)
        valid = self.Hv[m] if ns else np.zeros(0, bool)
        rs, rn, rt = {}, {}, {}
        cur = P[:ns] if encode else np.zeros((ns, ND), U)
        for col in ORDER:
            pred = predict(col, self.modes.get(col, "prev"), prev, prev2, valid, cur, m)
            if encode:
                rs[col] = (cur[:, col] - pred).astype(U)
            else:
                cur[:, col] = (pred + res[0][col]).astype(U)
        # new records: against the last new record of the same weapon (dword 0 raw)
        nnew = c - ns
        newrows = P[ns:c] if encode else np.zeros((nnew, ND), U)
        for col in range(ND):
            rn[col] = np.zeros(nnew, U)
        for i in range(nnew):
            if encode:
                w = int(newrows[i, 0]); rn[0][i] = w
            else:
                w = int(res[1][0][i]); newrows[i, 0] = w
            ref = self.lastnew.get(w, np.zeros(ND, U))
            for col in range(1, ND):
                if encode:
                    rn[col][i] = U(int(newrows[i, col]) - int(ref[col]) & 0xFFFFFFFF)
                else:
                    newrows[i, col] = U(int(ref[col]) + int(res[1][col][i]) & 0xFFFFFFFF)
            self.lastnew[w] = newrows[i].copy()
        # tail: against the slot's previous bytes
        trows = P[tail] if encode else np.zeros((len(tail), ND), U)
        olds = np.array([Q[j] if j < qhw else np.zeros(ND, U) for j in tail], U).reshape(len(tail), ND)
        for col in range(ND):
            if encode:
                rt[col] = (trows[:, col] - olds[:, col]).astype(U)
            else:
                trows[:, col] = (olds[:, col] + res[2][col]).astype(U)
        if encode:
            return rs, rn, rt
        return cur, newrows, trows

    def _advance(self, P, c, hw, m):
        ns = len(m)
        H = np.zeros((c, ND), U); Hv = np.zeros(c, bool)
        if ns:
            H[:ns] = self.Q[m]; Hv[:ns] = True
        self.H, self.Hv = H, Hv
        self.Q = P[:hw].copy(); self.qc = c; self.qhw = hw

    # ---- the decoder: from ctl and residuals, the pool
    def decode(self, ctl, rs, rn, rt, snap):
        if snap:
            self.Hv = np.zeros(len(self.Hv), bool)
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
        cur, newrows, trows = self._residuals(None, c, m, tail, Q, qhw, encode=False, res=(rs, rn, rt))
        P = np.zeros((hw, ND), U)
        n = min(hw, qhw)
        P[:n] = Q[:n]                    # the tail keeps its bytes unless listed
        P[:len(m)] = cur
        P[len(m):c] = newrows
        if tail:
            P[tail] = trows
        self._advance(P, c, hw, m)
        return P, c, hw

# ---- serialisation
def leb(vals):
    out = bytearray()
    for v in vals:
        v = int(v)
        while True:
            b = v & 0x7F; v >>= 7
            if v:
                out.append(b | 0x80)
            else:
                out.append(b); break
    return bytes(out)

def unleb(data, n, at=0):
    vals = []
    for _ in range(n):
        v = 0; s = 0
        while True:
            b = data[at]; at += 1
            v |= (b & 0x7F) << s; s += 7
            if not b & 0x80: break
        vals.append(v)
    return vals, at
