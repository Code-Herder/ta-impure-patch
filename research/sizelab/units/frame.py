"""frame.py -- a lossless unit-frame codec for the demo recording's unit deltas, offline.

One section per stream block (or per iteration): the iterations' changed units as slot gaps, a
lane mask per unit, then the residuals column by column (struct of arrays). A residual is the
lane's value less a prediction from the unit's two previous images (or one, after a reset):

    prev   the previous value (today's delta semantics)
    lin    2*prev - prevprev, mod the lane's width (constant rate: positions, counters, stamps)
    auto   per block and lane, whichever of the two leaves fewer nonzero residuals (1 bit/lane)
    rules  engine integrations on top: position += velocity(t), heading += turn rate(t), the
           mover's stamp = GameTime-1 when it moved, the countdowns max(prev-1, 0)

A unit is listed only when some residual is nonzero, so a unit moving at constant velocity costs
nothing. Created and gone units keep their records (RK_UNIT, RK_UNIT_GONE), outside this section.
Encode and decode are both here; decode() parses the bytes alone (plus the decoder's own state).
"""
import numpy as np

import sim

NBYTES = 0x148

# ---- lane tables ----------------------------------------------------------------------------


def lanes_dword():
    return [(o, 4) for o in range(0, NBYTES, 4)]


def lanes_u16():
    return [(o, 2) for o in range(0, NBYTES, 2)]


def lanes_field():
    """The unit record's own field boundaries where they are not dwords (the engine map, *The
    unit and player records, byte by byte*; the mover's from 0x43D6D0 and 0x43CD20). Only a
    compression hint: a wrong boundary costs bytes, never correctness."""
    spec = {}
    for i in range(3):                                     # the weapon slots
        b = 0x04 + 0x1C * i
        for o, n in ((0, 2), (2, 2), (4, 4), (8, 4), (0xC, 4), (0x10, 4), (0x14, 2), (0x16, 2),
                     (0x18, 2), (0x1A, 1), (0x1B, 1)):
            spec[b + o] = n
    for o, n in ((0x00, 4), (0x58, 4), (0x5C, 4), (0x60, 4), (0x64, 2), (0x66, 2), (0x68, 2),
                 (0x6A, 4), (0x6E, 4), (0x72, 4), (0x76, 2), (0x78, 2), (0x7A, 2), (0x7C, 2),
                 (0x7E, 2), (0x80, 2), (0x82, 4), (0x86, 4), (0x8A, 4), (0x8E, 4), (0x92, 4),
                 (0x96, 4), (0x9A, 4), (0x9E, 4), (0xA2, 4), (0xA6, 2), (0xA8, 2), (0xAA, 2),
                 (0xAC, 4), (0xB0, 4), (0xB4, 4), (0xB8, 2), (0xBA, 2)):
        spec[o] = n
    for o in range(0xBC, 0xF4, 4):
        spec[o] = 4
    for o, n in ((0xF4, 1), (0xF5, 1), (0xF6, 1), (0xF7, 1), (0xF8, 1), (0xF9, 1), (0xFA, 1),
                 (0xFB, 4), (0xFF, 1), (0x100, 4), (0x104, 4), (0x108, 2), (0x10A, 2),
                 (0x10C, 2), (0x10E, 1), (0x10F, 1), (0x110, 4), (0x114, 4)):
        spec[o] = n
    m = 0x118                                              # the mover
    for o, n in ((0, 4), (4, 4), (8, 4), (0xC, 4), (0x10, 4), (0x14, 4), (0x18, 4), (0x1C, 4),
                 (0x20, 4), (0x24, 2), (0x26, 2), (0x28, 2), (0x2A, 4), (0x2E, 1), (0x2F, 1)):
        spec[m + o] = n
    out, o = [], 0
    while o < NBYTES:
        n = spec[o]
        out.append((o, n))
        o += n
    assert sum(n for _, n in out) == NBYTES
    return out


LANES = {"dword": lanes_dword, "u16": lanes_u16, "field": lanes_field}


class Lanes:
    def __init__(self, table):
        self.table = table
        self.n = len(table)
        self.off = np.array([o for o, _ in table])
        self.size = np.array([s for _, s in table])
        self.mod = (1 << (8 * self.size)).astype(np.int64)
        self.half = self.mod >> 1
        self.index = {o: i for i, (o, _) in enumerate(table)}

    def split(self, S):
        """(rows, 82) uint32 -> (rows, nlanes) int64, each lane unsigned."""
        b = np.ascontiguousarray(S).view(np.uint8).reshape(len(S), NBYTES).astype(np.int64)
        out = np.zeros((len(S), self.n), np.int64)
        for k in range(4):
            sel = self.size > k
            out[:, sel] |= b[:, self.off[sel] + k] << (8 * k)
        return out

    def join(self, L):
        b = np.zeros((len(L), NBYTES), np.uint8)
        for k in range(4):
            sel = self.size > k
            b[:, self.off[sel] + k] = (L[:, sel] >> (8 * k)) & 0xFF
        return b.view("<u4").reshape(len(L), NBYTES // 4)

    def signed(self, R):
        R = R % self.mod
        return np.where(R >= self.half, R - self.mod, R)


# ---- varints --------------------------------------------------------------------------------

def varint(a):
    """LEB128 of non-negative ints, vectorized."""
    a = np.asarray(a, np.uint64)
    if not len(a):
        return b""
    nb = np.ones(len(a), np.int64)
    for k in range(1, 10):
        nb += a >= (np.uint64(1) << np.uint64(7 * k))
    out = np.zeros(int(nb.sum()), np.uint8)
    pos = np.concatenate(([0], np.cumsum(nb)[:-1]))
    for k in range(int(nb.max())):
        sel = nb > k
        byte = (a[sel] >> np.uint64(7 * k)) & np.uint64(0x7F)
        more = (nb[sel] > k + 1).astype(np.uint64) << np.uint64(7)
        out[pos[sel] + k] = (byte | more).astype(np.uint8)
    return out.tobytes()


def unvarint(buf, at, n):
    """n LEB128 ints from buf[at:]; returns (array, new at)."""
    if n == 0:
        return np.zeros(0, np.int64), at
    b = np.frombuffer(buf, np.uint8, offset=at)
    ends = np.flatnonzero(b < 0x80)[:n]
    end = int(ends[-1]) + 1
    b = b[:end].astype(np.int64)
    starts = np.concatenate(([0], ends[:-1] + 1))
    grp = np.repeat(np.arange(n), ends - starts + 1)
    shift = 7 * (np.arange(end) - starts[grp])
    vals = np.zeros(n, np.int64)
    np.add.at(vals, grp, (b & 0x7F) << shift)
    return vals, at + end


def zz(a):
    a = np.asarray(a, np.int64)
    return (a << 1) ^ (a >> 63)


def unzz(u):
    u = np.asarray(u, np.int64)
    return (u >> 1) ^ -(u & 1)


# ---- prediction -----------------------------------------------------------------------------

class Rules:
    """The engine's own integrations, as predictions of a lane from the CURRENT image's other
    lanes (decoded first) and the previous image. Each is a hint: a miss is a residual."""

    def __init__(self, ln):
        f = ln.index.get
        self.ln = ln
        self.pos = [(f(0x6A), f(0x118 + 0x08)), (f(0x72), f(0x118 + 0x10))]
        self.heading = (f(0x66), f(0x118 + 0x24))
        self.stamp = f(0x118 + 0x2A)
        self.layer = f(0x118 + 0x2E)
        self.count = [f(0x04 + 0x1C * i + 0x14) for i in range(3)] + [f(0xFA), f(0xFB)]
        self.ok = all(v is not None for v in (self.pos[0][0], self.pos[0][1], self.pos[1][0],
                                              self.heading[0], self.heading[1], self.stamp,
                                              self.layer)) and None not in self.count
        # the order a decoder must reconstruct in: everything else first, then these
        self.after = ([p for p, _ in self.pos] + [self.heading[0], self.stamp]) if self.ok else []

    def apply(self, P, L1, L0, gt):
        """P: the generic prediction (rows, lanes), overwritten where a rule applies; L0 holds
        the current image's lanes that the rules read (velocity, turn rate, layer)."""
        ln = self.ln
        for p, v in self.pos:
            P[:, p] = (L1[:, p] + np.where(L0[:, v] >= 1 << 31, L0[:, v] - (1 << 32), L0[:, v])) % ln.mod[p]
        h, t = self.heading
        P[:, h] = (L1[:, h] + L0[:, t]) % 65536
        moved = (L0[:, ln.index[0x118 + 0x08]] != 0) | (L0[:, ln.index[0x118 + 0x0C]] != 0) | \
                (L0[:, ln.index[0x118 + 0x10]] != 0) | ((L0[:, self.layer] & 3) != (L1[:, self.layer] & 3))
        P[:, self.stamp] = np.where(moved, (gt - 1) % (1 << 32), L1[:, self.stamp])
        for c in self.count:
            P[:, c] = np.maximum(L1[:, c] - 1, 0)
        return P


def predict(ln, L1, L2, lin_mask):
    """Per lane: lin (2*L1 - L2) where lin_mask, else L1."""
    P = np.where(lin_mask[None, :], (2 * L1 - L2) % ln.mod, L1)
    return P


# ---- the encoder ----------------------------------------------------------------------------

class Encoder:
    def __init__(self, lanes="field", pred="auto", rules=False, masks="dict", values="varint",
                 scope="block", reset_ticks=()):
        self.ln = Lanes(LANES[lanes]())
        self.pred, self.masks, self.values, self.scope = pred, masks, values, scope
        self.rules = Rules(self.ln) if rules else None
        self.reset = set(reset_ticks)
        self.L1 = np.zeros((sim.SLOTS, self.ln.n), np.int64)
        self.L2 = np.zeros((sim.SLOTS, self.ln.n), np.int64)
        self.prev_live = np.zeros(sim.SLOTS, bool)
        self.lastmask = {}
        self.Lsnap = np.zeros((sim.SLOTS, self.ln.n), np.int64)   # pred=snap: the base image

    def residuals(self, tk, lin_mask):
        """The iteration's (slots, residual rows) against the prediction, and the new lanes."""
        L0 = self.ln.split(tk.S)
        if tk.gametime - 1 in self.reset:
            self.L2[:] = self.L1
            self.Lsnap[:] = self.L1
        cand = tk.live & self.prev_live
        if tk.created:
            cand[tk.created] = False
        idx = np.flatnonzero(cand)
        P = self.Lsnap[idx] if self.pred == "snap" else predict(self.ln, self.L1[idx], self.L2[idx], lin_mask)
        if self.rules is not None:
            P = self.rules.apply(P, self.L1[idx], L0[idx], tk.gametime)
        R = self.ln.signed(L0[idx] - P)
        nz = (R != 0).any(axis=1)
        return idx[nz], R[nz], L0

    def advance(self, tk, L0):
        self.L2[:] = self.L1
        self.L1[:] = L0
        if tk.created:
            self.L2[tk.created] = L0[tk.created]
            self.Lsnap[tk.created] = L0[tk.created]
        self.prev_live = tk.live.copy()

    def block_bytes(self, frames, lin_mask):
        """frames: [(slots, R)] for the block's iterations -> bytes."""
        ln = self.ln
        out = [varint([len(frames)]), varint([len(f[0]) for f in frames])]
        gaps = [np.diff(np.concatenate(([-1], f[0]))) - 1 for f in frames]
        out.append(varint(np.concatenate(gaps)) if gaps else b"")
        allR = np.concatenate([f[1] for f in frames]) if frames else np.zeros((0, ln.n), np.int64)
        allS = np.concatenate([f[0] for f in frames]) if frames else np.zeros(0, np.int64)
        allV = np.concatenate([f[2] for f in frames]) if frames and len(frames[0]) > 2 else None
        M = allR != 0
        if self.pred == "auto":
            out.append(np.packbits(lin_mask, bitorder="little").tobytes())
        nb = (ln.n + 7) // 8
        packed = np.packbits(M, axis=1, bitorder="little") if len(M) else np.zeros((0, nb), np.uint8)
        if self.masks == "raw":
            out.append(packed.tobytes())
        elif self.masks == "dict":
            keys = [r.tobytes() for r in packed]
            uniq = {}
            ids = np.array([uniq.setdefault(k, len(uniq)) for k in keys], np.int64)
            out.append(varint([len(uniq)]))
            out.append(b"".join(uniq))
            out.append(ids.astype(np.uint8).tobytes() if len(uniq) <= 256 else varint(ids))
        elif self.masks == "mru":
            flags = np.zeros(len(allS), np.uint8)
            fresh = []
            for i, (s, k) in enumerate(zip(allS.tolist(), (r.tobytes() for r in packed))):
                if self.lastmask.get(s) == k:
                    flags[i] = 0
                else:
                    flags[i] = 1
                    fresh.append(k)
                    self.lastmask[s] = k
            out.append(np.packbits(flags, bitorder="little").tobytes())
            out.append(b"".join(fresh))
        if self.values in ("rows-abs", "rows-diff"):
            src = allV if self.values == "rows-abs" else allR
            v = (src[M] % (1 << 32)).astype("<u4")      # unit by unit, lanes ascending: dword lanes only
            out.append(v.tobytes())
            return b"".join(out)
        for j in range(ln.n):
            col = allR[M[:, j], j]
            if not len(col):
                continue
            if self.values == "varint":
                out.append(varint(zz(col)))
            elif self.values == "fixed":
                out.append((col % ln.mod[j]).astype("<u8").view(np.uint8).reshape(-1, 8)[:, :ln.size[j]].tobytes())
            elif self.values == "abs":
                v = allV[M[:, j], j]
                out.append((v % ln.mod[j]).astype("<u8").view(np.uint8).reshape(-1, 8)[:, :ln.size[j]].tobytes())
            elif self.values == "split":
                b = (col % ln.mod[j]).astype("<u8").view(np.uint8).reshape(-1, 8)[:, :ln.size[j]]
                out.append(b.T.tobytes())
        return b"".join(out)


def choose_lin(enc, ticks_of_block):
    """auto: per lane, lin when it leaves fewer nonzero residuals over the block than prev.
    Runs the block twice on copies of the encoder's state (the writer would buffer the block)."""
    ln = enc.ln
    counts = {}
    for mode in (False, True):
        L1, L2, pl = enc.L1.copy(), enc.L2.copy(), enc.prev_live.copy()
        nzc = np.zeros(ln.n, np.int64)
        for tk in ticks_of_block:
            _, R, L0 = enc.residuals(tk, np.full(ln.n, mode))
            nzc += (R != 0).sum(axis=0)
            enc.advance(tk, L0)
        counts[mode] = nzc
        enc.L1, enc.L2, enc.prev_live = L1, L2, pl
    return counts[True] < counts[False]
