"""frame3.py -- frame2's unit frame (research/sizelab/units/frame2.py) with the air step's rules.

The section format is frame2's, byte for byte: iterations, units listed, slot gaps, the mask
dictionary and ids, then one zigzag-varint column per lane.  Only the predictions change.

Units are in two groups, decided per row from the type lane (decoded first, generic): AIR (the
types whose movement runs the air step 0x43D290, def +0x241 bit 11 -- here the air-war's six
types, passed in) and the rest.  Each lane is decoded in an order that may differ per group,
so a rule reads only lanes decoded before it for that row:

    1. every lane that is generic in both groups                    (all rows)
    2. the air-only rule lanes, generic for the rest                (other rows)
    3. heading <- h(t-1) + turn rate                                (all rows)
    4. ground velocity <- -(sin, 0, cos)(h) x speed                 (other rows)
       air velocity <- the air step's known part + the PD estimate  (air rows)
    5. air: speed <- isqrt(v.v); A_s <- A_s*0xF333>>16 + dv; bank, roll <- atan2 (air rows)
    6. x, z <- + v (all rows); y <- + vy (air rows)
    7. footprint, sight cell (all rows); the claim stamp m26; the move stamp; the countdowns

Per (row, lane) the candidate is frame2's: the rule, prev or lin, whichever was exact last time
(else the closest; ties rule, prev, lin), so a wrong rule costs bytes, never correctness.
Engine facts: see air.md beside this folder.
"""
import numpy as np

import frame
import sim
from frame import Lanes, LANES, varint, unvarint, zz, unzz
from sintab import T as SINTAB

SIN = SINTAB.astype(np.int64)
M32 = 1 << 32
M = 0x118
K_BANK = 10430.37835047        # the double at 0x509EF0 (0x4B715A)
TH_ROT = 9.587379924285e-05    # the double at 0x509EF8 (0x4B7173)


def s32(a):
    a = np.asarray(a, np.int64) % M32
    return np.where(a >= 1 << 31, a - M32, a)


def s16(a):
    a = np.asarray(a, np.int64) & 0xFFFF
    return np.where(a >= 1 << 15, a - (1 << 16), a)


def mul_asr16(a, b):
    """_allmul then _allshr(16) (0x4E4400, 0x4E43D0): the 64-bit product shifted, low dword."""
    return s32((np.asarray(a, np.int64) * np.asarray(b, np.int64)) >> 16)


def clip(a, b=1 << 30):
    """Bound a value read from a record before it is squared or multiplied (int64 headroom)."""
    return np.clip(np.asarray(a, np.int64), -b, b)


def isqrt64(n):
    """floor(sqrt(n)) exactly for 0 <= n < 2**62."""
    n = np.asarray(n, np.int64)
    s = np.floor(np.sqrt(n.astype(np.float64))).astype(np.int64)
    s = np.where(s * s > n, s - 1, s)
    s = np.where((s + 1) * (s + 1) <= n, s + 1, s)
    return s


def sinmul(h, s):
    """0x4B70EF: (T[((h + 0x20) >> 7) & 511] * s + 0x1000) >> 13, low dword."""
    return s32((SIN[((np.asarray(h, np.int64) + 0x20) >> 7) & 511] * s + 0x1000) >> 13)


def cosmul(h, s):
    """0x4B7123: the same with h + 0x4020."""
    return s32((SIN[((np.asarray(h, np.int64) + 0x4020) >> 7) & 511] * s + 0x1000) >> 13)


class TypeTable:
    """Per type the def constants the air rules read (the writer would copy them from the defs):
    +0x192 maxvelocity, +0x19A brakerate, +0x19E acceleration, +0x1A2 bankscale, +0x1A6
    pitchscale; and the level's gravity main+0x14263."""

    def __init__(self, defs, gravity, sea=75):
        n = 65536
        self.sight_h = np.zeros(n, np.int64)
        self.sea = sea
        self.air = np.zeros(n, bool)
        self.maxv = np.ones(n, np.int64)
        self.brake = np.zeros(n, np.int64)
        self.acc = np.zeros(n, np.int64)
        self.bank = np.zeros(n, np.int64)
        self.pitch = np.zeros(n, np.int64)
        for t, d in defs.items():
            self.air[t] = True
            self.maxv[t], self.brake[t], self.acc[t] = d["maxv"], d["brake"], d["acc"]
            self.bank[t], self.pitch[t] = d["bank"], d["pitch"]
            self.sight_h[t] = d.get("sight_h", 0)
        self.q = (gravity << 16) // 0xCCD          # 0x43D186..0x43D1A4


def known_part(tt, typ, h, vx, vy, vz):
    """The air step before its vertical clamp and PD term (0x43D2F1..0x43D47D), from the velocity
    and heading the step starts with.  IEEE double and binary32 only: + - * / sqrt, no libm."""
    d = 0x10000 - (tt.acc[typ] << 16) // tt.maxv[typ]
    v1x, v1y, v1z = mul_asr16(vx, d), mul_asr16(vy, d), mul_asr16(vz, d)
    v1x, v1z = clip(v1x), clip(v1z)
    S = (v1x * v1x + v1z * v1z).astype(np.float64)                  # exact below 2**53
    m32 = (np.sqrt(S) * (1.0 / 65536)).astype(np.float32).astype(np.float64)
    c32 = (tt.brake[typ].astype(np.float64) * (1.0 / 65536)).astype(np.float32).astype(np.float64)
    over = ~(m32 <= c32)
    with np.errstate(divide="ignore", invalid="ignore"):
        k = np.where(over, np.trunc(c32 / np.where(m32 == 0, 1.0, m32) * 65536.0), 0).astype(np.int64)
    e = np.where(over, np.trunc((m32 - c32) * 65536.0), 0).astype(np.int64)
    k, e = s32(k), s32(e)
    v2x = np.where(over, s32(mul_asr16(v1x, k) - sinmul(h, e)), v1x)
    v2z = np.where(over, s32(mul_asr16(v1z, k) - cosmul(h, e)), v1z)
    return v2x, v1y, v2z


def bank_angle(ax, az, h, scale, q):
    """0x43D0D0's angle: rotate (A_s.x, A_s.z) by the heading (0x4B7173, skipped at heading 0),
    scale the x by the def's word (imul, >> 16), then round(atan2(that, q) * K) (0x4B715A)."""
    hs = s16(h).astype(np.float64) * TH_ROT
    xr = np.rint(ax.astype(np.float64) * np.cos(hs) - az.astype(np.float64) * np.sin(hs)).astype(np.int64)
    xr = np.where(h == 0, ax, xr)
    val = s32((-xr * scale) >> 16)
    return np.rint(np.arctan2(val.astype(np.float64), float(q)) * K_BANK).astype(np.int64)


def rot_extrapolate(x1, z1, x2, z2):
    """A1 turned by the angle from A2 to A1: A1 * (A1 conj(A2)) / (|A1| |A2|), numerators in
    int64, the normalisation and the rounding in IEEE double."""
    x1, z1, x2, z2 = (clip(a, 1 << 20) for a in (x1, z1, x2, z2))
    n1 = x1 * x1 + z1 * z1
    n2 = x2 * x2 + z2 * z2
    ok = (n1 > 0) & (n2 > 0)
    dot = x1 * x2 + z1 * z2
    cr = x2 * z1 - z2 * x1
    s = np.sqrt(np.where(ok, n1, 1).astype(np.float64)) * np.sqrt(np.where(ok, n2, 1).astype(np.float64))
    px = np.rint((x1 * dot - z1 * cr).astype(np.float64) / s).astype(np.int64)
    pz = np.rint((x1 * cr + z1 * dot).astype(np.float64) / s).astype(np.int64)
    return np.where(ok, px, x1), np.where(ok, pz, z1)


class Rules:
    def __init__(self, ln, tt, ruleset):
        i = ln.index
        self.ln, self.tt, self.rs = ln, tt, set(ruleset)
        L = lambda o: i[o]
        self.h, self.tr, self.sp = L(0x66), L(M + 0x24), L(M + 0x20)
        self.vx, self.vy, self.vz = L(M + 0x08), L(M + 0x0C), L(M + 0x10)
        self.ax, self.ay, self.az = L(M + 0x14), L(M + 0x18), L(M + 0x1C)
        self.x, self.y, self.z = L(0x6A), L(0x6E), L(0x72)
        self.bank, self.roll = L(0x64), L(0x68)
        self.fx, self.fz, self.fw, self.fh = L(0x76), L(0x78), L(0x7E), L(0x80)
        self.sx, self.sz = L(0x7A), L(0x7C)
        self.m26 = L(M + 0x26)
        self.stamp, self.layer, self.type = L(M + 0x2A), L(M + 0x2E), L(0xA6)
        self.counts = [i[0x04 + 0x1C * k + 0x14] for k in range(3)] + [i[0xFA], i[0xFB]]
        rs = self.rs
        # the steps: (lane, group, rule name); group "all", "air" or "gnd"
        common_early, common_late = [], []
        if "heading" in rs:
            common_early.append(self.h)
        gnd_v = [self.vx, self.vz] if "velocity" in rs else []
        air_v = ([self.vx, self.vz] if "air_v" in rs else []) + ([self.vy] if "air_vy" in rs else [])
        air_5 = ([self.sp] if "speed" in rs else []) + \
                ([self.ax, self.ay, self.az] if "accs" in rs else []) + \
                ([self.bank, self.roll] if "bank" in rs else [])
        pos_all = [self.x, self.z] if "position" in rs else []
        pos_air = [self.y] if "y" in rs else []
        late = ([self.fx, self.fz] if "foot" in rs else []) + ([self.sx] if "sight" in rs else []) + \
               ([self.m26] if "m26" in rs else []) + ([self.stamp] if "stamp" in rs else []) + \
               (self.counts if "counts" in rs else [])
        late_air = [self.sz] if "sight" in rs else []
        pos_air = pos_air + late_air
        rule_any = set(common_early + gnd_v + air_v + air_5 + pos_all + pos_air + late)
        self.generic_all = [j for j in range(ln.n) if j not in rule_any]
        gen_gnd = [j for j in rule_any if j not in set(common_early + gnd_v + pos_all + late)]
        gen_air = [j for j in rule_any if j not in set(common_early + air_v + air_5 + pos_all + pos_air + late)]
        S = [(j, "all", False) for j in self.generic_all]
        S += [(j, "gnd", False) for j in sorted(gen_gnd)] + [(j, "air", False) for j in sorted(gen_air)]
        S += [(j, "all", True) for j in common_early]
        S += [(j, "gnd", True) for j in gnd_v] + [(j, "air", True) for j in air_v]
        S += [(j, "air", True) for j in air_5]
        S += [(j, "all", True) for j in pos_all] + [(j, "air", True) for j in pos_air if j not in late_air]
        S += [(j, "all", True) for j in late] + [(j, "air", True) for j in late_air]
        self.steps = S
        self.rule_lanes = rule_any
        self.vz_given_vx = "vz_sat" in rs
        self.rot2 = "air_rot2" in rs

    # -- candidates ---------------------------------------------------------------------------
    def candidate(self, lane, grp, L1, L2, L3, L0, age, gt, cache):
        ln, tt = self.ln, self.tt
        if lane == self.h:
            return (L1[:, self.h] + L0[:, self.tr]) % 65536
        if lane in (self.x, self.z, self.y):
            v = s32(L0[:, {self.x: self.vx, self.z: self.vz, self.y: self.vy}[lane]])
            return (L1[:, lane] + v) % M32
        if lane in (self.fx, self.fz):
            p = s32(L0[:, self.x if lane == self.fx else self.z])
            w = s16(L0[:, self.fw if lane == self.fx else self.fh])
            return ((p - (w << 19) + 0x80000) >> 20) % 65536           # 0x43D86F..0x43D895
        if lane == self.sx:
            return (s32(L0[:, self.x]) >> 21) % 65536                 # the sight emit 0x4825D4..0x4825E1
        if lane == self.sz:
            # 0x4825D8..0x48261C: (z - h/2) >> 5, h = clamp(altitude raised to the sea + 1, + def +0x170)
            y = np.maximum(s32(L0[:, self.y]), (tt.sea + 1) << 16)
            hgt = np.clip(s16(y >> 16) + tt.sight_h[L0[:, self.type]], 0, 255)
            cz = ((s32(L0[:, self.z]) >> 16) - (hgt >> 1)) >> 5
            if "sight_edge" in self.rs:
                # a cell north of the map's edge mostly keeps the old one [MEASURED, writer not traced];
                # worth nothing measurable over per-unit switching (f1b against f1)
                return np.where(cz < 0, L1[:, self.sz], cz % 65536)
            return cz % 65536
        if lane == self.m26:
            moved = (L0[:, self.fx] != L1[:, self.fx]) | (L0[:, self.fz] != L1[:, self.fz]) | \
                    ((L0[:, self.layer] & 3) != (L1[:, self.layer] & 3))
            return np.where(moved, (gt - 1) % 65536, L1[:, self.m26])
        if lane == self.stamp:
            moved = (L0[:, self.vx] != 0) | (L0[:, self.vy] != 0) | (L0[:, self.vz] != 0) | \
                ((L0[:, self.layer] & 3) != (L1[:, self.layer] & 3))
            return np.where(moved, (gt - 1) % M32, L1[:, self.stamp])
        if lane in self.counts:
            return np.maximum(L1[:, lane] - 1, 0)
        if grp == "gnd":
            if lane in (self.vx, self.vz):
                h, s = L0[:, self.h], s32(L0[:, self.sp])
                idx = ((h + (0x20 if lane == self.vx else 0x4020)) >> 7) & 511
                return (-((SIN[idx] * s + 0x1000) >> 13)) % M32
            raise KeyError(lane)
        # -- the air rules ------------------------------------------------------------------
        typ = L0[:, self.type]
        air_now = (L0[:, self.layer] & 3) == 2
        if lane in (self.vx, self.vz, self.vy):
            if "vel" not in cache:
                cache["vel"] = self._air_velocity(typ, L1, L2, L3, L0, age)
                if self.rot2:
                    cache["rot"] = self._rot
            vx, vz, vy = cache["vel"]
            if lane in (self.vx, self.vz):
                if lane == self.vz and self.vz_given_vx:
                    vz = self._vz_given_vx(typ, L0, cache, vz)
                r1 = np.where(air_now, vx if lane == self.vx else vz, 0) % M32
                if not self.rot2:
                    return r1
                rx_, rz_ = cache["rot"]
                return r1, np.where(air_now, rx_ if lane == self.vx else rz_, 0) % M32
            return np.where(air_now, vy, 0) % M32
        if lane == self.sp:
            vx, vy, vz = (clip(s32(L0[:, j])) for j in (self.vx, self.vy, self.vz))
            return np.where(air_now, isqrt64(vx * vx + vy * vy + vz * vz), 0) % M32
        if lane in (self.ax, self.ay, self.az):
            v = {self.ax: self.vx, self.ay: self.vy, self.az: self.vz}[lane]
            a = mul_asr16(s32(L1[:, lane]), 0xF333) + s32(L0[:, v]) - s32(L1[:, v])
            return np.where(air_now, a, L1[:, lane]) % M32
        if lane in (self.bank, self.roll):
            scale = (tt.bank if lane == self.bank else tt.pitch)[typ]
            b = bank_angle(s32(L0[:, self.ax]), s32(L0[:, self.az]), L0[:, self.h], scale, tt.q)
            return np.where(air_now, b, L1[:, lane]) % 65536
        raise KeyError(lane)

    def _air_velocity(self, typ, L1, L2, L3, L0, age):
        tt = self.tt
        sv = lambda L, j: s32(L[:, j])
        kx, ky, kz = known_part(tt, typ, L1[:, self.h], sv(L1, self.vx), sv(L1, self.vy), sv(L1, self.vz))
        # A(t-1) and A(t-2): the PD terms of the last two steps, from decoded images
        l1 = (L1[:, self.layer] & 3) == 2
        l2 = (L2[:, self.layer] & 3) == 2
        l3 = (L3[:, self.layer] & 3) == 2
        qx, _, qz = known_part(tt, typ, L2[:, self.h], sv(L2, self.vx), sv(L2, self.vy), sv(L2, self.vz))
        rx, _, rz = known_part(tt, typ, L3[:, self.h], sv(L3, self.vx), sv(L3, self.vy), sv(L3, self.vz))
        ok1 = (age >= 2) & l1 & l2
        ok2 = ok1 & (age >= 3) & l3
        a1x = np.where(ok1, s32(sv(L1, self.vx) - qx), 0)
        a1z = np.where(ok1, s32(sv(L1, self.vz) - qz), 0)
        a2x = np.where(ok2, s32(sv(L2, self.vx) - rx), 0)
        a2z = np.where(ok2, s32(sv(L2, self.vz) - rz), 0)
        if "air_rot" in self.rs:
            px, pz = rot_extrapolate(a1x, a1z, a2x, a2z)
        else:
            px, pz = a1x, a1z
        if self.rot2:
            ox, oz = rot_extrapolate(a1x, a1z, a2x, a2z)
            self._rot = (s32(kx + ox), s32(kz + oz))
        self._apred = (px, pz)
        self._known = (kx, kz)
        # vertical: continue a saturated climb or descent at the new limit, else hold level
        sp1, sp2 = sv(L1, self.sp), sv(L2, self.sp)
        c1 = np.where((sp1 & ~3) < 0x40000, 0x10000, sp1 >> 2)
        c2 = np.where((sp2 & ~3) < 0x40000, 0x10000, sp2 >> 2)
        vy1 = sv(L1, self.vy)
        vy = np.where((vy1 == c2) & (age >= 2), c1, np.where((vy1 == -c2) & (age >= 2), -c1, 0))
        return s32(kx + px), s32(kz + pz), vy

    def _vz_given_vx(self, typ, L0, cache, vz):
        """When the decoded vx missed the estimate, the PD term was saturated at |A| = acc with
        another direction: take the z that completes it, with the estimate's sign."""
        kx, kz = self._known
        px, pz = self._apred
        ax = s32(s32(L0[:, self.vx]) - kx)
        acc = self.tt.acc[typ]
        r = isqrt64(np.maximum(acc * acc - ax * ax, 0))
        alt = s32(kz + np.where(pz >= 0, r, -r))
        return np.where(ax == px, vz, alt)


class Codec:
    def __init__(self, tt, ruleset, lanes="field", reset_ticks=()):
        self.ln = Lanes(LANES[lanes]())
        self.rules = Rules(self.ln, tt, ruleset)
        self.reset = set(reset_ticks)
        n = self.ln.n
        self.default = np.full(n, 1, np.int8)
        self.default[sorted(self.rules.rule_lanes)] = 0     # a rule lane starts on its rule (frame2)
        self.L1 = np.zeros((sim.SLOTS, n), np.int64)
        self.L2 = np.zeros((sim.SLOTS, n), np.int64)
        self.L3 = np.zeros((sim.SLOTS, n), np.int64)
        self.age = np.zeros(sim.SLOTS, np.int64)
        self.W = np.tile(self.default, (sim.SLOTS, 1))
        self.prev_live = np.zeros(sim.SLOTS, bool)
        self.tt = tt

    def begin(self, gt, live, created):
        if gt - 1 in self.reset:
            self.L2[:] = self.L1
            self.L3[:] = self.L1
            self.age[:] = np.minimum(self.age, 1)
            self.W[:] = self.default
        cand = live & self.prev_live
        if created:
            cand[created] = False
        return np.flatnonzero(cand)

    def _run(self, idx, gt, L0, R, decode):
        """Evaluate every step for rows idx: encode fills R from L0; decode fills L0 from R."""
        ln, rl = self.ln, self.rules
        L1, L2, L3, W, age = self.L1[idx], self.L2[idx], self.L3[idx], self.W[idx].copy(), self.age[idx]
        air = None
        cache = {}
        for j, grp, is_rule in rl.steps:
            if grp == "all":
                rows = slice(None)
            else:
                if air is None:
                    air = self.tt.air[L0[:, rl.type]]
                    ai, gi = np.flatnonzero(air), np.flatnonzero(~air)
                    sub = lambda A, r: A[r]
                    cache_air = {}
                rows = ai if grp == "air" else gi
                if not len(rows):
                    continue
            mod = ln.mod[j]
            l1, l2 = L1[rows, j], L2[rows, j]
            lin = (2 * l1 - l2) % mod
            extra = []
            if is_rule:
                rule = rl.candidate(j, grp, L1[rows], L2[rows], L3[rows], L0[rows], age[rows], gt,
                                    cache_air if grp == "air" else {})
                if isinstance(rule, tuple):            # a second rule: candidate 3
                    rule, r2 = rule
                    extra = [r2 % mod]
                rule = rule % mod
            else:
                rule = l1
            C = np.stack([rule, l1, lin] + extra)
            w = W[rows, j].astype(np.int64)
            pred = np.take_along_axis(C, w[None, :], 0)[0]
            if decode:
                L0[rows, j] = (pred + R[rows, j]) % mod
            else:
                r = (L0[rows, j] - pred) % mod
                R[rows, j] = np.where(r >= ln.half[j], r - mod, r)
            actual = L0[rows, j]
            err = (actual[None, :] - C) % mod
            err = np.minimum(err, mod - err)
            if not is_rule:
                err[0] = np.iinfo(np.int64).max
            W[rows, j] = np.argmin(err, axis=0)
        self.W[idx] = W

    def end(self, live, created, L0rows_all):
        self.L3[:] = self.L2
        self.L2[:] = self.L1
        self.L1[:] = L0rows_all
        self.age = np.minimum(self.age + 1, 3)
        if created:
            self.L2[created] = L0rows_all[created]
            self.L3[created] = L0rows_all[created]
            self.age[created] = 1
            self.W[created] = self.default
        self.age[~live] = 0
        self.prev_live = live.copy()

    def encode_tick(self, tk):
        ln = self.ln
        L0all = ln.split(tk.S)
        idx = self.begin(tk.gametime, tk.live, tk.created)
        L0 = L0all[idx]
        R = np.zeros((len(idx), ln.n), np.int64)
        self._run(idx, tk.gametime, L0, R, decode=False)
        nz = (R != 0).any(axis=1)
        self.end(tk.live, tk.created, L0all)
        return idx[nz], R[nz]

    block_bytes = None   # set below from frame2's (identical format)

    def decode_block(self, buf, ticks_meta):
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
        Mk = np.unpackbits(dic, axis=1, bitorder="little")[:, :ln.n].astype(bool)[ids] if total else \
            np.zeros((0, ln.n), bool)
        R = np.zeros((total, ln.n), np.int64)
        for j in range(ln.n):
            k = int(Mk[:, j].sum())
            if k:
                v, at = unvarint(buf, at, k)
                R[Mk[:, j], j] = unzz(v)
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
            L0 = np.zeros((len(idx), ln.n), np.int64)
            self._run(idx, gt, L0, Rrows, decode=True)
            L0all[idx] = L0
            L0all[~live] = 0
            self.end(live, created, L0all)
            out.append(L0all.copy())
        return out


import frame2 as _f2  # noqa: E402
Codec.block_bytes = _f2.Codec.block_bytes


# -- the challenge's all-integer variant: the bank by intbank (Q31 fixed point, no floating point)
# and the turned candidate with operands bounded below 2**16 and an integer rounding division.
from intbank import bank_int as _bank_int


def bank_angle(ax, az, h, scale, q):                    # noqa: F811
    return _bank_int(ax, az, h, scale, q)


def rot_extrapolate(x1, z1, x2, z2):                    # noqa: F811
    """A1 turned by the angle from A2 to A1, in int64: (A1 * (A1 conj A2)) / |A2|^2, rounded half
    up by floor division; operands clipped to 2**16 so every product stays below 2**51."""
    x1, z1, x2, z2 = (clip(a, 1 << 16) for a in (x1, z1, x2, z2))
    n2 = x2 * x2 + z2 * z2
    ok = (n2 > 0) & ((x1 != 0) | (z1 != 0))
    dot = x1 * x2 + z1 * z2
    cr = x2 * z1 - z2 * x1
    d = np.where(ok, n2, 1)
    px = (2 * (x1 * dot - z1 * cr) + d) // (2 * d)
    pz = (2 * (x1 * cr + z1 * dot) + d) // (2 * d)
    return np.where(ok, px, x1), np.where(ok, pz, z1)
