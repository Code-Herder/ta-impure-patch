"""frame2.py -- the unit frame with per-unit predictor switching, encoder AND decoder.

A block's unit section (one per stream block):

    varint  iterations
    varint  units listed, per iteration
    varint  slot gaps, per listed unit (slot - previous slot - 1; the first from -1)
    varint  distinct lane masks; the masks (ceil(lanes/8) bytes each); a u8 (or varint when
            more than 256) mask index per listed unit
    then for each lane in order: zigzag varint residuals of the listed units whose mask has it

A unit is listed when some lane's residual is nonzero. Every lane's prediction is one of its
candidates, picked per (unit, lane) by the candidate that was exact last iteration (first in
priority order; when none was, the one closest) -- state both sides derive from decoded values,
so no side information. Candidates:

    generic:  prev (L1), lin (2*L1 - L2) mod the lane's width, prev first on a tie
    rules:    engine integrations from lanes decoded earlier in the same iteration (Rules.ORDER)

The decoder's state resets (L2 := L1, winners := default) at the first iteration after a
snapshot, where a reader may start; a created unit starts with L2 = L1 = its image.
"""
import numpy as np

import frame
import sim
from frame import Lanes, LANES, varint, unvarint, zz, unzz
from sintab import T as SINTAB

SIN = SINTAB.astype(np.int64)
M32 = 1 << 32


def s32(a):
    a = a % M32
    return np.where(a >= 1 << 31, a - M32, a)


class Rules:
    """Engine integrations, each a candidate prediction for one lane from lanes decoded before it.
    DISASSEMBLED: heading += mover turn rate (0x43CFD1..0x43CFDB); velocity = -(sin, 0, cos) of the
    heading times the speed, by the 512-entry table at 0x509F00 (0x4B70EF, 0x4B7123); position +=
    velocity (0x43D7FF..0x43D839); the mover's +0x2A = GameTime when the position or layer changed
    (0x43D85A..0x43D865); the countdowns max(v-1, 0) (reload 0x49E1C8, +0xFA and +0xFB 0x48ADF0)."""

    def __init__(self, ln, ruleset=("heading", "velocity", "position", "stamp", "counts")):
        i = ln.index
        self.ln = ln
        M = 0x118
        self.h, self.tr, self.sp = i[0x66], i[M + 0x24], i[M + 0x20]
        self.vx, self.vy, self.vz = i[M + 0x08], i[M + 0x0C], i[M + 0x10]
        self.x, self.y, self.z = i[0x6A], i[0x6E], i[0x72]
        self.stamp, self.layer = i[M + 0x2A], i[M + 0x2E]
        self.counts = [i[0x04 + 0x1C * k + 0x14] for k in range(3)] + [i[0xFA], i[0xFB]]
        # decode order: these lanes after every other, in this order (each reads only earlier ones)
        groups = {"heading": [self.h], "velocity": [self.vx, self.vz], "position": [self.x, self.z],
                  "stamp": [self.stamp], "counts": self.counts}
        self.order = [j for g in ("heading", "velocity", "position", "stamp", "counts")
                      if g in ruleset for j in groups[g]]

    def candidate(self, lane, L1, L0, gt):
        """The rule's prediction for `lane`, given L1 (previous lanes) and L0 (this iteration's,
        valid for every lane earlier in the order)."""
        ln = self.ln
        if lane == self.h:
            return (L1[:, self.h] + L0[:, self.tr]) % 65536
        if lane in (self.vx, self.vz):
            h, s = L0[:, self.h], s32(L0[:, self.sp])
            idx = ((h + (0x20 if lane == self.vx else 0x4020)) >> 7) & 511
            return (-((SIN[idx] * s + 0x1000) >> 13)) % M32
        if lane in (self.x, self.z):
            v = s32(L0[:, self.vx if lane == self.x else self.vz])
            return (L1[:, lane] + v) % M32
        if lane == self.stamp:
            moved = (L0[:, self.vx] != 0) | (L0[:, self.vy] != 0) | (L0[:, self.vz] != 0) | \
                ((L0[:, self.layer] & 3) != (L1[:, self.layer] & 3))
            return np.where(moved, (gt - 1) % M32, L1[:, self.stamp])
        if lane in self.counts:
            return np.maximum(L1[:, lane] - 1, 0)
        raise KeyError(lane)


class Codec:
    def __init__(self, lanes="field", rules=True, pred="win", reset_ticks=(), ruleset=None):
        self.ln = Lanes(LANES[lanes]())
        self.rules = (Rules(self.ln, ruleset) if ruleset else Rules(self.ln)) if rules else None
        self.pred = pred
        self.reset = set(reset_ticks)
        n = self.ln.n
        self.rule_lanes = list(self.rules.order) if self.rules else []
        self.generic = [j for j in range(n) if j not in set(self.rule_lanes)]
        # candidates 0 rule (rule lanes only), 1 prev, 2 lin: on a tie the earlier wins, so a
        # lane that stopped changing settles on prev and a one-off step costs one residual
        self.default = np.full(n, 1, np.int8)
        if self.rules:
            self.default[self.rule_lanes] = 0
        self.L1 = np.zeros((sim.SLOTS, n), np.int64)
        self.L2 = np.zeros((sim.SLOTS, n), np.int64)
        self.W = np.tile(self.default, (sim.SLOTS, 1))
        self.prev_live = np.zeros(sim.SLOTS, bool)

    # -- the prediction, shared by both sides --------------------------------------------------
    def begin(self, gt, live, created):
        if gt - 1 in self.reset:
            self.L2[:] = self.L1
            self.W[:] = self.default
        cand = live & self.prev_live
        if created:
            cand[created] = False
        return np.flatnonzero(cand)

    def lane_pred(self, j, idx, L1, L2, W, L0, gt):
        """(prediction, all candidates) of lane j for rows idx. L0 must hold the rule inputs."""
        mod = self.ln.mod[j]
        lin = (2 * L1[:, j] - L2[:, j]) % mod
        prev = L1[:, j]
        if self.pred == "prev":
            return prev, None
        if self.pred == "lin":
            return lin, None
        if self.rules and j in self.rules.counts or (self.rules and j in self.rule_lanes):
            rule = self.rules.candidate(j, L1, L0, gt) % mod
        else:
            rule = prev                                  # never chosen for a generic lane
        C = np.stack([rule, prev, lin])
        return np.take_along_axis(C, W[:, j][None, :].astype(np.int64), 0)[0], C

    def winners(self, j, C, actual, W):
        if C is None:
            return
        mod = self.ln.mod[j]
        err = (actual[None, :] - C) % mod
        err = np.minimum(err, mod - err)
        if not self.rules or j not in set(self.rule_lanes):
            err[0] = np.iinfo(np.int64).max             # generic lanes have no rule candidate
        W[:, j] = np.argmin(err, axis=0)                # exact first; ties by priority order

    def end(self, live, created, L0rows_all):
        """Advance: L2 = L1, L1 = this iteration's lanes."""
        self.L2[:] = self.L1
        self.L1[:] = L0rows_all
        if created:
            self.L2[created] = L0rows_all[created]
            self.W[created] = self.default
        self.prev_live = live.copy()

    # -- encode --------------------------------------------------------------------------------
    def encode_tick(self, tk):
        ln = self.ln
        L0all = ln.split(tk.S)
        idx = self.begin(tk.gametime, tk.live, tk.created)
        L1, L2, W, L0 = self.L1[idx], self.L2[idx], self.W[idx].copy(), L0all[idx]
        R = np.zeros((len(idx), ln.n), np.int64)
        for j in self.generic + self.rule_lanes:
            p, C = self.lane_pred(j, idx, L1, L2, W, L0, tk.gametime)
            r = (L0[:, j] - p) % ln.mod[j]
            R[:, j] = np.where(r >= ln.half[j], r - ln.mod[j], r)
            self.winners(j, C, L0[:, j], W)
        self.W[idx] = W
        nz = (R != 0).any(axis=1)
        self.end(tk.live, tk.created, L0all)
        return idx[nz], R[nz]

    def block_bytes(self, frames):
        ln = self.ln
        out = [varint([len(frames)]), varint([len(s) for s, _ in frames])]
        out.append(varint(np.concatenate([np.diff(np.concatenate(([-1], s))) - 1 for s, _ in frames])))
        R = np.concatenate([r for _, r in frames]) if frames else np.zeros((0, ln.n), np.int64)
        M = R != 0
        packed = np.packbits(M, axis=1, bitorder="little")
        uniq = {}
        ids = np.array([uniq.setdefault(r.tobytes(), len(uniq)) for r in packed], np.int64)
        out.append(varint([len(uniq)]))
        out.append(b"".join(uniq))
        out.append(ids.astype(np.uint8).tobytes() if len(uniq) <= 256 else varint(ids))
        for j in range(ln.n):
            col = R[M[:, j], j]
            if len(col):
                out.append(varint(zz(col)))
        return b"".join(out)

    # -- decode --------------------------------------------------------------------------------
    def decode_block(self, buf, ticks_meta):
        """ticks_meta: [(gametime, live, created, gone, S_after_creates)] per iteration -- the
        RK_UNIT/RK_UNIT_GONE records the decoder applies itself, outside this section. Returns the
        decoded lanes of every slot after each iteration."""
        ln = self.ln
        nb = (ln.n + 7) // 8
        at = 0
        (nt,), at = unvarint(buf, at, 1)
        nun, at = unvarint(buf, at, int(nt))
        total = int(nun.sum())
        gaps, at = unvarint(buf, at, total)
        (nd,), at = unvarint(buf, at, 1)
        dic = np.frombuffer(buf, np.uint8, int(nd) * nb, at).reshape(int(nd), nb)
        at += int(nd) * nb
        if nd <= 256:
            ids = np.frombuffer(buf, np.uint8, total, at).astype(np.int64)
            at += total
        else:
            ids, at = unvarint(buf, at, total)
        M = np.unpackbits(dic, axis=1, bitorder="little")[:, :ln.n].astype(bool)[ids] if total else \
            np.zeros((0, ln.n), bool)
        R = np.zeros((total, ln.n), np.int64)
        for j in range(ln.n):
            k = int(M[:, j].sum())
            if k:
                v, at = unvarint(buf, at, k)
                R[M[:, j], j] = unzz(v)
        assert at == len(buf), (at, len(buf))
        out = []
        u0 = 0
        for (gt, live, created, gone, Screate), n in zip(ticks_meta, nun):
            n = int(n)
            slots = np.cumsum(gaps[u0:u0 + n] + 1) - 1
            Rt = R[u0:u0 + n]
            u0 += n
            L0all = self.L1.copy()
            if created:
                L0all[created] = ln.split(Screate[created])
            idx = self.begin(gt, live, created)
            pos = np.searchsorted(idx, slots)
            assert np.array_equal(idx[pos], slots), "a listed slot outside the candidates"
            Rrows = np.zeros((len(idx), ln.n), np.int64)
            Rrows[pos] = Rt
            L1, L2, W = self.L1[idx], self.L2[idx], self.W[idx].copy()
            L0 = np.zeros((len(idx), ln.n), np.int64)
            for j in self.generic + self.rule_lanes:
                p, C = self.lane_pred(j, idx, L1, L2, W, L0, gt)
                L0[:, j] = (p + Rrows[:, j]) % ln.mod[j]
                self.winners(j, C, L0[:, j], W)
            self.W[idx] = W
            L0all[idx] = L0
            L0all[~live] = 0
            self.end(live, created, L0all)
            out.append(L0all.copy())
        return out
