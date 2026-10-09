"""codec4.py -- codec3.py plus two engine facts for the records a step appends:
  EVENT  the step's RK_EV_SHOT events (already in the stream, before the next iteration's pool)
         are extra reference candidates, each laid over the last new record of its weapon: the
         weapon (+0x19 and the high nibble of +0x23, plus one), the fire point (+0x01) as the
         position and the tail (+0x04, +0x10: MEASURED tail == fire point in 25,566 of 25,566
         launches), the target point (+0x0D -> +0x28), the target (+0x1F -> +0x4E) and the shooter
         (+0x21 -> +0x52);
  XPOS   a new record's position is predicted from its own tail and velocity, decoded first:
         tail + velocity + the reference's drift (position - tail - velocity), or the tail when
         the reference's position is its tail (a burst template, which does not move)."""
import struct
import numpy as np
import codec as C
import codec2 as C2
import codec3 as C3
from codec import ND, U

def ev_row(b, base):
    row = base.copy()
    w = b[0x19] | ((b[0x23] >> 4) << 8)
    row[0] = w + 1
    s = struct.unpack_from("<3I", b, 1); t = struct.unpack_from("<3I", b, 0x0D)
    row[1:4] = s; row[4:7] = s; row[10:13] = t
    raw = bytearray(row.astype("<u4").tobytes())
    raw[0x4E:0x52] = struct.pack("<I", struct.unpack_from("<H", b, 0x1F)[0])
    raw[0x52:0x56] = struct.pack("<I", struct.unpack_from("<H", b, 0x21)[0])
    return np.frombuffer(bytes(raw), "<u4").astype(U)

def xpos(row, ref):
    """the position predicted from the record's own tail and velocity (columns 4..9 decoded)"""
    r = ref.astype(np.int64); x = row.astype(np.int64)
    out = np.zeros(3, np.int64)
    for a in range(3):
        if r[1 + a] == r[4 + a]:
            out[a] = x[4 + a]
        else:
            out[a] = x[4 + a] + x[7 + a] + (r[1 + a] - r[4 + a] - r[7 + a])
    return (out % 2**32).astype(U)

def xpos_all(row, T):
    """xpos for every candidate row of T at once"""
    r = T.astype(np.int64); x = row.astype(np.int64)
    out = np.where(r[:, 1:4] == r[:, 4:7], x[4:7][None, :],
                   x[4:7][None, :] + x[7:10][None, :] + (r[:, 1:4] - r[:, 4:7] - r[:, 7:10]))
    return (out % 2**32).astype(U)

class Codec4(C3.Codec3):
    def __init__(self, stamp=True, event=True, xp=True):
        super().__init__(stamp)
        self.event, self.xp = event, xp
        self.shots = []

    def set_shots(self, shots):
        self.shots = shots

    def _code(self, P, c, m, tail, encode, res=None):
        # survivors and the tail exactly as codec3; the new records redone here
        Q, qc, qhw = self.Q, self.qc, self.qhw
        ns = len(m)
        saved = None
        if encode:
            rs, rn0, rt, refs0 = super()._code(P, c, m, tail, True)
            ntail = len(tail)
            refs_tail = refs0[len(refs0) - ntail:] if ntail else refs0[:0]
        else:
            # decode survivors and tail with codec3 (its new-record part is redone below)
            pass
        # the candidates both sides hold for the new records
        prev = Q[m] if ns else np.zeros((0, ND), U)
        nnew = c - ns
        out = np.zeros((c, ND), U)
        if encode:
            out[:ns] = P[:ns]
        else:
            out_s, trows = super()._code(None, c, m, tail, False,
                                         (res[0], {col: np.zeros(nnew, U) for col in range(ND)},
                                          res[2], np.concatenate([np.zeros(nnew, U), res[3][nnew:]])))
            out[:ns] = out_s[:ns]
        adv = self.advanced(Q, qc)
        evrows = []
        if self.event:
            for b in self.shots:
                w = (b[0x19] | ((b[0x23] >> 4) << 8)) + 1
                evrows.append(ev_row(b, self.lastnew.get(w, np.zeros(ND, U))))
        evrows = np.array(evrows, U).reshape(len(evrows), ND)
        rn = {col: np.zeros(nnew, U) for col in range(ND)}
        refs = []
        def table(n_out):
            return np.concatenate([out[:n_out], Q[:qhw], adv, evrows])
        POSC = (1, 2, 3)
        for i in range(nnew):
            Tb = table(ns + i)
            if self.stamp:
                s = qc + i
                Tb = Tb.copy()
                Tb[:, 25] = (Tb[:, 25].astype(np.int64) & 0x00FFFFFF) | ((s & 0xFF) << 24)
                Tb[:, 26] = (Tb[:, 26].astype(np.int64) & 0xFFFFFF00) | ((s >> 8) & 0xFF)
            guess = ns + i - 1 if i > 0 else max(ns - 1, 0)
            if encode:
                row = P[ns + i]
                if not len(Tb):
                    ref = np.zeros(ND, U); refs.append(0)
                else:
                    if self.xp:   # the position's cost is its residual against xpos
                        pp = xpos_all(row, Tb)
                        cand = Tb.copy(); cand[:, 1:4] = pp
                        r = int(np.argmin(C2.cost(cand, row)))
                    else:
                        r = C2.search(Tb, row, ns + i, qc, qhw)
                    ref = Tb[r]
                    refs.append(int(C.zz(np.array([(r - guess) % 2**32], U))[0]) + 1)
                pred = ref.copy()
                if self.xp:
                    pred[1:4] = xpos(row, ref)
                for col in range(ND):
                    rn[col][i] = U((int(row[col]) - int(pred[col])) & 0xFFFFFFFF)
                out[ns + i] = row
            else:
                z = int(res[3][i])
                if z == 0:
                    ref = np.zeros(ND, U)
                else:
                    r = (guess + int(C.unzz(np.array([z - 1], U))[0])) % 2**32
                    ref = Tb[r]
                row = np.zeros(ND, U)
                for col in range(ND):
                    if col not in POSC or not self.xp:
                        row[col] = U((int(ref[col]) + int(res[1][col][i])) & 0xFFFFFFFF)
                if self.xp:
                    p = xpos(row, ref)
                    for a, col in enumerate(POSC):
                        row[col] = U((int(p[a]) + int(res[1][col][i])) & 0xFFFFFFFF)
                out[ns + i] = row
            self.lastnew[int(out[ns + i, 0])] = out[ns + i].copy()
        if encode:
            return rs, rn, rt, np.concatenate([np.array(refs, U), refs_tail])
        # the tail was decoded against the table with the new records' placeholders: redo it
        # with the real ones (codec3's tail references index the table [out | Q | adv])
        Tt = np.concatenate([out[:c], Q[:qhw], adv])
        newidx = np.full(qc, -1, np.int64); newidx[m] = np.arange(ns)
        trows = np.zeros((len(tail), ND), U)
        for t, j in enumerate(tail):
            if j < qc:
                guess = int(newidx[j]) if newidx[j] >= 0 else c + qhw + j
            else:
                k = ns + (j - qc)
                guess = k if k < c else j
            z = int(res[3][nnew + t])
            r = (guess + int(C.unzz(np.array([z - 1], U))[0])) % 2**32
            trows[t] = (Tt[r] + np.array([res[2][col][t] for col in range(ND)], U)).astype(U)
        return out, trows
