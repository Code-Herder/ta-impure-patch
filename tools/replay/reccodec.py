"""reccodec -- a recording's stream codec, read side: format 8's block walk, the sectioned
container and the unit frame, in numpy.

    import reccodec
    for blk in reccodec.iter_blocks(path):     # every complete block, stream payloads decoded
        ...

THE FORMAT is tagpu/ddraw/inc/tagpu_recfmt.h (RBF_SECTIONS, RecSecHead, RCS_*, RT_CODEC, RT_PHYSICS,
RT_AIR) and the codec tagpu/ddraw/src/tagpu_reccodec.c, which the writer and the DLL's reader run.
This file is an INDEPENDENT implementation of its decoder, not a port to be kept in step line by
line: tagpu/ddraw/tests/codectest.py decodes every block of a recording with both and requires the
same bytes, and feeds both the same mutated blocks and requires the same accept or refuse. The
rules, their engine facts and their bounds are written down beside the C; here only what differs
in the doing.

A stream block with RBF_SECTIONS decodes to the records it was made from; one without is the
records themselves. The unit frame's state crosses blocks inside a run of sectioned blocks and
restarts at RBF_FRAME_RESET from this reader's own copy of the units, which is why the walk is in
order and per segment: a block that continues a frame this reading did not start is refused.

INTEGERS ONLY: int64 arrays, `>>` a floor shift, `//` a floor division (equal to C's where both
operands are not negative, which is everywhere C divides), the same clamps as the C, so that no
product reaches 2^63. The sine table is computed from its formula at import, never read from the
executable.

Needs numpy, and zstandard for a packed block (the shared venv; devpy.ensure() re-executes a
tool under it). zstandard is imported where a block is unpacked, so a walk with decode=False over
a file whose only packed blocks are its stream, snapshots and minimap runs without it.
"""
import importlib.util
import math
import os
import struct
import sys
import zlib
from collections import namedtuple

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import tarec  # noqa: E402  the record kinds and Unreadable


def _lanes_generator():
    spec = importlib.util.spec_from_file_location("replay_lanes_gen",
                                                  os.path.join(HERE, "..", "replay-lanes-gen.py"))
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    return gen


_GEN = _lanes_generator()
ROLES = _GEN.ROLES                         # (name, width) by role number: the one copy
ROLE = {name: i for i, (name, _) in enumerate(ROLES)}
NROLES = len(ROLES)

Unreadable = tarec.Unreadable

REC_FORMAT = 8
RBF_STORED, RBF_SECTIONS, RBF_FRAME_RESET = 1, 2, 4
RT_CODEC, RT_PHYSICS, RT_AIR = 29, 30, 31
RT_CODEC_DECLINED, RT_CODEC_MISMATCH = 44, 45
(RCS_KINDS, RCS_UNITS, RCS_PROJ, RCS_CELLS, RCS_ORDERS, RCS_EVENTS, RCS_OTHER) = range(7)
RCS_COUNT = 7
LANES_MAX, COUNTS_MAX, AIR_MAX, TICKS_MAX = 128, 8, 1024, 256
FRAME_CAP = 16 << 20
PAYLOAD_MAX = 64 << 20                     # RECR_BLOCK_MAX
IMG = tarec.REC_UNIT_IMAGE                 # 0x148
NDW = IMG // 4
DELTA_HEAD = tarec.REC_UNIT_DELTA_HEAD     # 16
RK_TICK, RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE = tarec.RK_TICK, tarec.RK_UNIT, tarec.RK_UNIT_DELTA, tarec.RK_UNIT_GONE

SIN = np.array([round(8192 * math.sin(2 * math.pi * i / 512)) for i in range(512)], np.int64)
ONE = 1 << 31
TH47, PI4, PI2, K47, ATAN_BIG = 13493037705, 1686629713, 3373259426, 683565276, 889516847
SINC = [0, 0, -54, 5918, -426088, 17895697, -357913941, 2147483648]
COSC = [0, 4, -592, 53261, -2982616, 89478485, -1073741824, 2147483648]
ATC = [74051160, -79536431, 85899346, -93368854, 102261126, -113025455, 126322568, -143165577,
       165191050, -195225786, 238609294, -306783378, 429496730, -715827883, 2147483648]

# the steps' lane groups, by role (tagpu_reccodec.c frame_slot)
_S1 = {ROLE[r] for r in ("PLAIN", "TURN", "FW", "FH", "LAYER", "TYPE")}
_S2 = {ROLE[r] for r in ("VY", "SPEED", "AX", "AY", "AZ", "BANK", "ROLL", "Y", "SZ")}


def section_of(kind):
    if kind == RK_UNIT_DELTA:
        return RCS_UNITS
    if kind in (tarec.RK_PROJ_HEAD, tarec.RK_PROJ, tarec.RK_PROJ_DELTA):
        return RCS_PROJ
    if kind == tarec.RK_CELLS:
        return RCS_CELLS
    if kind == tarec.RK_ORDERS:
        return RCS_ORDERS
    if tarec.RK_EV_CREATE <= kind <= tarec.RK_EV_DEATH_END or kind == tarec.RK_EV_NET_CREATE:
        return RCS_EVENTS
    return RCS_OTHER


# ---- the header's entries ---------------------------------------------------------------------

Params = namedtuple("Params", "nslots lanes gravity sea sight air usable")
AirRow = namedtuple("AirRow", "type sight_h maxv brake acc bank pitch")


def params(entries, nslots):
    """A segment's codec constants from its header's (tag, body) entries; Unreadable when one is
    malformed, past a bound or given twice. `usable`: RT_CODEC and RT_PHYSICS are both there."""
    got = {}
    for tag, body in entries:
        if tag in (RT_CODEC, RT_PHYSICS, RT_AIR):
            if tag in got:
                raise Unreadable(f"header tag {tag} twice")
            got[tag] = bytes(body)
    if not 2 <= nslots <= 0xFFFF:
        raise Unreadable(f"RT_SLOTS {nslots} is outside 2..65535")
    lanes, gravity, sea, sight, air = [], 0, 0, 0, []
    b = got.get(RT_CODEC)
    if b is not None:
        if len(b) < 4:
            raise Unreadable(f"RT_CODEC is {len(b)} bytes, under its 4-byte head")
        n, pad = struct.unpack_from("<HH", b)
        if pad:
            raise Unreadable("RT_CODEC's pad is not zero")
        if not 1 <= n <= LANES_MAX:
            raise Unreadable(f"RT_CODEC has {n} rows, not 1..{LANES_MAX}")
        if len(b) != 4 + 4 * n:
            raise Unreadable(f"RT_CODEC is {len(b)} bytes, not {4 + 4 * n} for {n} rows")
        at, seen, counts = 0, set(), 0
        for i in range(n):
            off, width, role = struct.unpack_from("<HBB", b, 4 + 4 * i)
            if off != at:
                raise Unreadable(f"RT_CODEC row {i} starts at {off:#x}, not {at:#x}")
            if width not in (1, 2, 4):
                raise Unreadable(f"RT_CODEC row {i} is {width} bytes wide")
            if at + width > IMG:
                raise Unreadable(f"RT_CODEC row {i} runs past the image")
            if role >= NROLES:
                raise Unreadable(f"RT_CODEC row {i} has role {role}")
            if ROLES[role][1] and width != ROLES[role][1]:
                raise Unreadable(f"RT_CODEC row {i}: role {ROLES[role][0]} is {ROLES[role][1]} bytes wide")
            if role == ROLE["COUNT"]:
                counts += 1
                if counts > COUNTS_MAX:
                    raise Unreadable(f"RT_CODEC row {i} is COUNT row {counts}")
            elif role != ROLE["PLAIN"]:
                if role in seen:
                    raise Unreadable(f"RT_CODEC row {i} repeats role {ROLES[role][0]}")
                seen.add(role)
            lanes.append((off, width, role))
            at += width
        if at != IMG:
            raise Unreadable(f"RT_CODEC's rows cover {at:#x} bytes, not {IMG:#x}")
        missing = [ROLES[r][0] for r in range(1, ROLE["COUNT"]) if r not in seen]
        if missing:
            raise Unreadable(f"RT_CODEC has no row for {', '.join(missing)}")
    b = got.get(RT_PHYSICS)
    if b is not None:
        if len(b) != 8:
            raise Unreadable(f"RT_PHYSICS is {len(b)} bytes, not 8")
        gravity, sea, sight, pad = struct.unpack("<iBBH", b)
        if sight > 1:
            raise Unreadable(f"RT_PHYSICS's sight rule is {sight}")
        if pad:
            raise Unreadable("RT_PHYSICS's pad is not zero")
    b = got.get(RT_AIR)
    if b is not None:
        if len(b) < 8:
            raise Unreadable(f"RT_AIR is {len(b)} bytes, under its 8-byte head")
        n, pad = struct.unpack_from("<II", b)
        if pad:
            raise Unreadable("RT_AIR's pad is not zero")
        if n > AIR_MAX:
            raise Unreadable(f"RT_AIR has {n} rows, past {AIR_MAX}")
        if len(b) != 8 + 24 * n:
            raise Unreadable(f"RT_AIR is {len(b)} bytes, not {8 + 24 * n}")
        for i in range(n):
            row = AirRow(*struct.unpack_from("<HBxiiiii", b, 8 + 24 * i))
            if b[8 + 24 * i + 3]:
                raise Unreadable(f"RT_AIR row {i}'s pad is not zero")
            if air and row.type <= air[-1].type:
                raise Unreadable(f"RT_AIR row {i}'s type {row.type} does not follow {air[-1].type}")
            air.append(row)
    return Params(nslots, lanes, gravity, sea, sight, air,
                  RT_CODEC in got and RT_PHYSICS in got)


# ---- integer arithmetic (the C's, vectorized; its bounds hold here) ---------------------------

def s32(a):
    return ((np.asarray(a, np.int64) + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


def s16(a):
    return ((np.asarray(a, np.int64) + (1 << 15)) & 0xFFFF) - (1 << 15)


def mul_asr16(a, b):
    return s32((a * b) >> 16)


def mulq(a, b):
    return (a * b) >> 31


def isqrt(n):
    """floor(sqrt(n)) exactly for 0 <= n < 2^62: a float estimate, corrected once each way."""
    n = np.asarray(n, np.int64)
    s = np.floor(np.sqrt(n.astype(np.float64))).astype(np.int64)
    s = np.where(s * s > n, s - 1, s)
    return np.where((s + 1) * (s + 1) <= n, s + 1, s)


def bitlen(a):
    a = np.asarray(a, np.int64).copy()
    n = np.zeros(a.shape, np.int64)
    for k in (32, 16, 8, 4, 2, 1):
        t = a >> k
        sel = t > 0
        a = np.where(sel, t, a)
        n += np.where(sel, k, 0)
    return n + (a > 0)


def round24_sqrt(S):
    b = bitlen(isqrt(S))
    k = np.maximum(26 - b, 0)
    T = S << (2 * k)
    r = isqrt(T)
    exact = r * r == T
    sh = np.maximum(bitlen(r) - 24, 0)
    m = r >> sh
    rem = r - (m << sh)
    half = np.where(sh > 0, 1 << np.maximum(sh - 1, 0), 0)
    up = (sh > 0) & ((rem > half) | ((rem == half) & (~exact | ((m & 1) == 1))))
    m = m + up
    ov = (m >> 24) > 0
    return np.where(ov, m >> 1, m), sh + ov - k


def table_mul(h, e, add):
    return s32((SIN[((h + add) >> 7) & 511] * e + 0x1000) >> 13)


def known(acc, maxv, brake, h, vx, vy, vz):
    d = np.maximum(0x10000 - (acc * 65536) // maxv, -(1 << 17))
    v1x = np.clip(mul_asr16(vx, d), -(1 << 30), 1 << 30)
    v1z = np.clip(mul_asr16(vz, d), -(1 << 30), 1 << 30)
    v1y = mul_asr16(vy, d)
    m, e = round24_sqrt(v1x * v1x + v1z * v1z)
    ep, en = np.maximum(e, 0), np.maximum(-e, 0)
    over = np.where(e >= 0, (m << ep) > brake, m > (brake << en))
    kf = s32(np.where(e >= 0, (brake * 65536) // np.maximum(m << ep, 1),
                      ((brake * 65536) << en) // np.maximum(m, 1)))
    ex = s32(np.where(e >= 0, m << ep, m >> en) - brake)
    v2x = np.where(over, s32(mul_asr16(v1x, kf) - table_mul(h, ex, 0x20)), v1x)
    v2z = np.where(over, s32(mul_asr16(v1z, kf) - table_mul(h, ex, 0x4020)), v1z)
    return v2x, v1y, v2z


def sin_cos_q31(h):
    h = h & 0xFFFF
    n = ((h + 0x2000) >> 14) & 3
    r = ((h + 0x2000) & 0x3FFF) - 0x2000
    x = (r * TH47) >> 16
    x2 = mulq(x, x)
    s = np.full(x.shape, SINC[0], np.int64)
    for c in SINC[1:]:
        s = mulq(s, x2) + c
    s = mulq(s, x)
    c_ = np.full(x.shape, COSC[0], np.int64)
    for c in COSC[1:]:
        c_ = mulq(c_, x2) + c
    sn = np.choose(n, [s, c_, -s, -c_])
    cs = np.choose(n, [c_, -s, -c_, s])
    return sn, cs


def atan_q31(u):
    big = u > ATAN_BIG
    w = np.where(big, ((u - ONE) * ONE) // (u + ONE), u)
    w2 = mulq(w, w)
    a = np.full(w.shape, ATC[0], np.int64)
    for c in ATC[1:]:
        a = mulq(a, w2) + c
    a = mulq(a, w)
    return np.where(big, PI4 + a, a)


def bank_int(ax, az, h, scale, q):
    ax = np.clip(ax, -(1 << 30), 1 << 30)
    az = np.clip(az, -(1 << 30), 1 << 30)
    sn, cs = sin_cos_q31(h)
    xr = np.where((h & 0xFFFF) == 0, ax, (ax * cs - az * sn + (1 << 30)) >> 31)
    val = s32((-xr * scale) >> 16)
    a = np.abs(val)
    swap = a > q
    num, den = np.where(swap, q, a), np.where(swap, a, q)
    u = (num * ONE) // np.maximum(den, 1)
    ang = atan_q31(u)
    ang = np.where(swap, PI2 - ang, ang)
    ang = np.where(val < 0, -ang, ang)
    return (ang * K47 + (1 << 46)) >> 47


def rot(x1, z1, x2, z2):
    x1, z1, x2, z2 = (np.clip(v, -(1 << 16), 1 << 16) for v in (x1, z1, x2, z2))
    n2 = x2 * x2 + z2 * z2
    ok = (n2 > 0) & ((x1 != 0) | (z1 != 0))
    d = np.where(ok, n2, 1)
    dot = x1 * x2 + z1 * z2
    cr = x2 * z1 - z2 * x1
    px = (2 * (x1 * dot - z1 * cr) + d) // (2 * d)
    pz = (2 * (x1 * cr + z1 * dot) + d) // (2 * d)
    return np.where(ok, px, x1), np.where(ok, pz, z1)


# ---- varints ----------------------------------------------------------------------------------

def _varint(buf, at, end):
    """One varint from buf[at:end] as the C reads it; (value, new at) or None."""
    x = 0
    for i in range(5):
        if at >= end:
            return None
        b = buf[at]
        at += 1
        if i == 4 and b > 0x0F:
            return None
        x |= (b & 0x7F) << (7 * i)
        if b < 0x80:
            return x, at
    return None


def _varints(buf, at, end, n):
    """n varints in a row from buf[at:end], each read as the C reads it; (values, new at) or
    None when one is malformed or runs past `end`."""
    if n == 0:
        return np.zeros(0, np.int64), at
    b = np.frombuffer(buf, np.uint8, end - at, at)
    term = np.flatnonzero(b < 0x80)
    if len(term) < n:
        return None
    term = term[:n]
    starts = np.empty(n, np.int64)
    starts[0] = 0
    starts[1:] = term[:-1] + 1
    ln = term - starts + 1
    if (ln > 5).any() or (b[term[ln == 5]] > 0x0F).any():
        return None
    used = int(term[-1]) + 1
    bb = b[:used].astype(np.int64)
    grp = np.repeat(np.arange(n), ln)
    shift = 7 * (np.arange(used) - starts[grp])
    vals = np.zeros(n, np.int64)
    np.add.at(vals, grp, (bb & 0x7F) << shift)
    return vals, at + used


# ---- the unit frame ---------------------------------------------------------------------------

class Frame:
    """One reading's unit frame: the copy of the units (L1), L2, L3's five lanes, the winners,
    the history's age and the resting flags, for a segment's params. `rest=False` evaluates every
    candidate every iteration, which must give the same bytes (the fixed-point argument)."""

    def __init__(self, prm, rest=True):
        self.prm = prm
        self.rest_mode = rest
        n = prm.nslots
        self.copy = np.zeros((n, IMG), np.uint8)
        self.live = np.zeros(n, bool)
        self.valid = False
        self._derive()

    def clone(self):
        f = Frame.__new__(Frame)
        f.__dict__.update(self.__dict__)
        for k in ("copy", "live", "l2", "l3", "win", "age", "rest"):
            if hasattr(self, k):
                setattr(f, k, getattr(self, k).copy())
        return f

    def _derive(self):
        p = self.prm
        self.nl = len(p.lanes)
        if not self.nl:
            return
        self.off = np.array([o for o, _, _ in p.lanes], np.int64)
        self.width = np.array([w for _, w, _ in p.lanes], np.int64)
        self.role = np.array([r for _, _, r in p.lanes], np.int64)
        self.mod = np.int64(1) << (8 * self.width)
        self.lane = {int(r): j for j, r in enumerate(self.role) if r not in (ROLE["PLAIN"], ROLE["COUNT"])}
        self.counts = [j for j, r in enumerate(self.role) if r == ROLE["COUNT"]]
        self.s1 = np.array([j for j, r in enumerate(self.role) if r in _S1], np.int64)
        self.s2 = np.array([j for j, r in enumerate(self.role) if r in _S2], np.int64)
        self.defwin = np.array([1 if r in _S1 else 0 for r in self.role], np.int8)
        self.q = (min(max(p.gravity, 0), 1 << 24) * 65536) // 0xCCD
        self.airix = np.full(65536, -1, np.int64)
        for i, a in enumerate(p.air):
            self.airix[a.type] = i
        A = p.air
        cl = lambda v, lo, hi: np.array([min(max(x, lo), hi) for x in v] or [0], np.int64)
        self.a_maxv = cl([a.maxv for a in A], 1, 1 << 24)
        self.a_brake = cl([a.brake for a in A], 0, 1 << 24)
        self.a_acc = cl([a.acc for a in A], 0, 1 << 24)
        self.a_bank = cl([a.bank for a in A], -(1 << 24), 1 << 24)
        self.a_pitch = cl([a.pitch for a in A], -(1 << 24), 1 << 24)
        self.a_sight = np.array([a.sight_h for a in A] or [0], np.int64)
        # the gather that cuts images into lanes
        self._gather = [(np.flatnonzero(self.width > k), k) for k in range(4)]

    def split(self, img):
        out = np.zeros((len(img), self.nl), np.int64)
        for sel, k in self._gather:
            out[:, sel] |= img[:, self.off[sel] + k].astype(np.int64) << (8 * k)
        return out

    def join(self, L):
        img = np.zeros((len(L), IMG), np.uint8)
        for sel, k in self._gather:
            img[:, self.off[sel] + k] = (L[:, sel] >> (8 * k)) & 0xFF
        return img

    def _l3_of(self, L):
        g = self.lane
        return np.stack([L[:, g[ROLE["HEADING"]]], L[:, g[ROLE["LAYER"]]], L[:, g[ROLE["VX"]]],
                         L[:, g[ROLE["VY"]]], L[:, g[ROLE["VZ"]]]], axis=1)

    def restart(self, prm=None):
        """The frame restarts from this reading's own copy of the units."""
        if prm is not None:
            self.prm = prm
            self._derive()
        self.valid = False
        if not self.prm.usable or not self.nl:
            return
        n = self.prm.nslots
        self.copy[~self.live] = 0
        L = self.split(self.copy)
        self.l2 = L
        self.l3 = self._l3_of(L)
        self.win = np.tile(self.defwin, (n, 1))
        self.age = self.live.astype(np.int8)
        self.rest = np.zeros(n, bool)
        self.valid = True

    def invalidate(self):
        self.valid = False

    def apply_records(self, payload):
        """A records block's RK_UNIT, RK_UNIT_DELTA and RK_UNIT_GONE, by the writer's rules."""
        at, n = 0, len(payload)
        while n - at >= 4:
            kind, ln = struct.unpack_from("<HH", payload, at)
            rl = (4 + ln + 3) & ~3
            if rl > n - at:
                break
            b = at + 4
            s = struct.unpack_from("<H", payload, b)[0] if ln >= 2 else 0
            if 1 <= s < self.prm.nslots:
                if kind == RK_UNIT and ln == 4 + IMG:
                    self.copy[s] = np.frombuffer(payload, np.uint8, IMG, b + 4)
                    self.live[s] = True
                elif kind == RK_UNIT_GONE and ln == 4:
                    self.live[s] = False
                elif kind == RK_UNIT_DELTA and ln >= DELTA_HEAD and self.live[s]:
                    bits = np.unpackbits(np.frombuffer(payload, np.uint8, 11, b + 2), bitorder="little")
                    idx = np.flatnonzero(bits)
                    if (idx < NDW).all() and ln == DELTA_HEAD + 4 * len(idx):
                        row = self.copy[s].view("<u4")
                        row[idx] = np.frombuffer(payload, "<u4", len(idx), b + DELTA_HEAD)
            at += rl

    def decode(self, flags, body):
        """A stream block's bytes (unpacked) to its records."""
        if flags & ~(RBF_STORED | RBF_SECTIONS | RBF_FRAME_RESET):
            raise Unreadable(f"block flags {flags:#x}")
        if flags & RBF_FRAME_RESET and not flags & RBF_SECTIONS:
            raise Unreadable("RBF_FRAME_RESET without RBF_SECTIONS")
        if not flags & RBF_SECTIONS:
            if self.prm.usable:             # the copy feeds only a restart, which needs the params
                self.apply_records(body)
            self.invalidate()
            return bytes(body)
        if not self.prm.usable:
            raise Unreadable("a sectioned block in a segment without RT_CODEC and RT_PHYSICS")
        if flags & RBF_FRAME_RESET:
            self.restart()
        if not self.valid:
            raise Unreadable("a stream block that continues a unit frame this reading did not start")
        self.valid = False
        out = self._decode(bytes(body))
        self.valid = True
        return out

    # -- the container --------------------------------------------------------------------------
    def _decode(self, sec):
        n = len(sec)
        if n < 36:
            raise Unreadable(f"a sectioned block of {n} bytes, under its 36-byte head")
        payload, iters, nsec = struct.unpack_from("<IHH", sec)
        lens = struct.unpack_from("<7I", sec, 8)
        if nsec != RCS_COUNT:
            raise Unreadable(f"{nsec} sections")
        off = [36]
        for ln in lens:
            if ln > n - off[-1]:
                raise Unreadable("a section runs past the block")
            off.append(off[-1] + ln)
        if off[-1] != n:
            raise Unreadable("the sections do not fill the block")
        if payload < 4 or payload % 4 or payload > PAYLOAD_MAX:
            raise Unreadable(f"a payload of {payload} bytes")
        if not 1 <= iters <= TICKS_MAX:
            raise Unreadable(f"{iters} iterations")
        # the kinds
        runs, at, end, records, ticks = [], off[RCS_KINDS], off[RCS_KINDS + 1], 0, 0
        while at < end:
            r = _varint(sec, at, end)
            r2 = r and _varint(sec, r[1], end)
            if not r2:
                raise Unreadable("a kind run cut short")
            (kind, _), (count, at) = r, r2
            if not 1 <= kind <= 0xFFFF:
                raise Unreadable(f"record kind {kind}")
            if count < 1:
                raise Unreadable("a run of no records")
            if not records and kind != RK_TICK:
                raise Unreadable("the first record is not RK_TICK")
            if count > payload // 4 - records:
                raise Unreadable("more records than the payload holds")
            records += count
            if kind == RK_TICK:
                ticks += count
            runs.append((kind, count))
        if ticks != iters:
            raise Unreadable(f"{ticks} RK_TICK records, not {iters} iterations")
        kinds = np.repeat(np.array([k for k, _ in runs], np.int64), [c for _, c in runs])
        # the bodies: each section's (offset, length) entries in order, as many as its kinds hold
        secof = np.array([section_of(int(k)) for k, _ in runs], np.int64)
        secs = np.repeat(secof, [c for _, c in runs])
        bodies = {}
        for s in range(RCS_PROJ, RCS_COUNT):
            want = int((secs == s).sum())
            at, end, ents = off[s], off[s + 1], []
            for _ in range(want):
                r = _varint(sec, at, end)
                if r is None or r[0] > 0xFFFF or r[0] > end - r[1]:
                    raise Unreadable(f"a body cut short or past 0xFFFF in section {s}")
                ln, at = r
                ents.append((at, ln))
                at += ln
            if at != end:
                raise Unreadable(f"{end - at} bytes left in section {s}")
            bodies[s] = ents
        frame = self._units_parse(sec, off[RCS_UNITS], off[RCS_UNITS + 1], iters)
        return self._iterations(sec, payload, kinds.tolist(), secs.tolist(), bodies, frame)

    def _units_parse(self, sec, at, end, iters):
        nl, nslots = self.nl, self.prm.nslots
        mb = (nl + 7) // 8
        r = _varint(sec, at, end)
        if r is None or r[0] != iters:
            raise Unreadable("the unit frame's iterations are not the head's")
        at = r[1]
        r = _varints(sec, at, end, iters)
        if r is None:
            raise Unreadable("the listed counts cut short")
        listed, at = r
        if (listed > nslots).any():
            raise Unreadable("an iteration lists past the slots")
        total = int(listed.sum())
        r = _varints(sec, at, end, total)
        if r is None:
            raise Unreadable("the slot gaps cut short")
        gaps, at = r
        slots = []
        u0 = 0
        for c in listed.tolist():
            sl = np.cumsum(gaps[u0:u0 + c] + 1)
            if c and sl[-1] >= nslots:
                raise Unreadable("a listed slot past the slots")
            slots.append(sl)
            u0 += c
        r = _varint(sec, at, end)
        if r is None:
            raise Unreadable("the mask count cut short")
        nd, at = r
        if (not 1 <= nd <= total) if total else nd != 0:
            raise Unreadable(f"{nd} masks for {total} listed units")
        if nd * mb > end - at:
            raise Unreadable("the masks run past the unit frame")
        dic = np.frombuffer(sec, np.uint8, nd * mb, at).reshape(nd, mb)
        at += nd * mb
        if nd and (~dic.any(axis=1)).any():
            raise Unreadable("an empty mask")
        if nd and nl % 8 and (dic[:, mb - 1] >> (nl % 8)).any():
            raise Unreadable("a mask names a lane past the table")
        if nd <= 256:
            if total > end - at:
                raise Unreadable("the mask ids cut short")
            ids = np.frombuffer(sec, np.uint8, total, at).astype(np.int64)
            at += total
        else:
            r = _varints(sec, at, end, total)
            if r is None:
                raise Unreadable("the mask ids cut short")
            ids, at = r
        if total and (ids >= nd).any():
            raise Unreadable("a mask id past the masks")
        M = (np.unpackbits(dic, axis=1, bitorder="little")[:, :nl].astype(bool)[ids] if total
             else np.zeros((0, nl), bool))
        R = np.zeros((total, nl), np.int64)
        cnt = M.sum(axis=0)
        r = _varints(sec, at, end, int(cnt.sum()))
        if r is None:
            raise Unreadable("a column cut short")
        vals, at = r
        if at != end:
            raise Unreadable(f"{end - at} bytes left in the unit frame")
        c0 = 0
        for j in range(nl):
            k = int(cnt[j])
            if not k:
                continue
            z = vals[c0:c0 + k]
            c0 += k
            if self.width[j] < 4 and (z >> (8 * int(self.width[j]))).any():
                raise Unreadable(f"lane {j}'s residual is past its width")
            R[M[:, j], j] = ((z >> 1) ^ -(z & 1)) % self.mod[j]
        return listed, slots, R

    def _iterations(self, sec, payload, kinds, secs, bodies, frame):
        listed, slots, R = frame
        out = bytearray()
        nslots = self.prm.nslots
        cursor = {s: 0 for s in bodies}
        k, nrec, u0 = 0, len(kinds), 0
        for it in range(len(listed)):
            # pass A: the iteration's records; their bodies are taken in order
            recs, touched, ndelta, gt = [], [], 0, 0
            first = True
            while k < nrec and (first or kinds[k] != RK_TICK):
                first = False
                kind, s = int(kinds[k]), int(secs[k])
                k += 1
                if kind == RK_UNIT_DELTA:
                    ndelta += 1
                    recs.append((kind, None, 0))
                    continue
                at, ln = bodies[s][cursor[s]]
                cursor[s] += 1
                if kind == RK_TICK:
                    if ln != 4:
                        raise Unreadable(f"an RK_TICK of {ln} bytes")
                    gt = struct.unpack_from("<I", sec, at)[0]
                elif kind in (RK_UNIT, RK_UNIT_GONE):
                    if ln != (4 + IMG if kind == RK_UNIT else 4):
                        raise Unreadable(f"a unit record of {ln} bytes")
                    slot = struct.unpack_from("<H", sec, at)[0]
                    if not 1 <= slot < nslots:
                        raise Unreadable(f"a unit record for slot {slot}")
                    touched.append(slot)
                recs.append((kind, at, ln))
            ls = slots[it]
            Rit = R[u0:u0 + len(ls)]
            u0 += len(ls)
            deltas = self._frame(ls, Rit, touched, gt)
            if len(deltas) != ndelta:
                raise Unreadable(f"iteration {it}: the frame changes {len(deltas)} units, its kinds hold {ndelta} deltas")
            # pass B: the records, in order
            d = 0
            for kind, at, ln in recs:
                if kind == RK_UNIT_DELTA:
                    body = deltas[d]
                    d += 1
                    ln = len(body)
                else:
                    body = sec[at:at + ln]
                    if kind == RK_UNIT:
                        s = body[0] | body[1] << 8
                        self.copy[s] = np.frombuffer(body, np.uint8, IMG, 4)
                        self.live[s] = True
                    elif kind == RK_UNIT_GONE:
                        self.live[body[0] | body[1] << 8] = False
                rl = (4 + ln + 3) & ~3
                if rl > payload - len(out):
                    raise Unreadable("the records pass the head's payload")
                out += struct.pack("<HH", kind, ln)
                out += body
                out += bytes(rl - 4 - ln)
            # the end of the iteration for the slots its records named
            if touched:
                t = np.unique(np.array(touched, np.int64))
                lv = t[self.live[t]]
                if len(lv):
                    L = self.split(self.copy[lv])
                    self.l2[lv] = L
                    self.l3[lv] = self._l3_of(L)
                    self.win[lv] = self.defwin
                    self.age[lv] = 1
                self.age[t[~self.live[t]]] = 0
                self.rest[t] = False
        for s in bodies:
            if cursor[s] != len(bodies[s]):
                raise Unreadable(f"records left in section {s}")
        if len(out) != payload:
            raise Unreadable(f"the records are {len(out)} bytes, not the head's {payload}")
        return bytes(out)

    # -- one iteration's frame ------------------------------------------------------------------
    def _frame(self, ls, Rit, touched, gt):
        nslots, g, ROLEX = self.prm.nslots, self.lane, ROLE
        cand = self.live.copy()
        cand[0] = False
        if touched:
            cand[np.array(touched, np.int64)] = False
        if len(ls) and not cand[ls].all():
            raise Unreadable("a listed slot that is not a candidate")
        ev = cand if not self.rest_mode else cand & ~self.rest
        if len(ls):
            ev = ev.copy()
            ev[ls] = True
        rows = np.flatnonzero(ev)
        if not len(rows):
            return []
        n, nl = len(rows), self.nl
        old = self.copy[rows]
        L1 = self.split(old)
        L2 = self.l2[rows]
        L3 = self.l3[rows]
        W = self.win[rows].astype(np.int64)
        age = self.age[rows].astype(np.int64)
        Rr = np.zeros((n, nl), np.int64)
        if len(ls):
            Rr[np.searchsorted(rows, ls)] = Rit
        U = np.zeros((n, nl), np.int64)
        mod = self.mod

        def generic(idx, lanes):
            if not len(idx) or not len(lanes):
                return
            ix = np.ix_(idx, lanes)
            m = mod[lanes]
            l1, l2 = L1[ix], L2[ix]
            lin = (2 * l1 - l2) % m
            w = W[ix]
            u = (np.where(w >= 2, lin, l1) + Rr[ix]) % m
            e1 = np.minimum((u - l1) % m, (l1 - u) % m)
            e2 = np.minimum((u - lin) % m, (lin - u) % m)
            U[ix] = u
            W[ix] = np.where(e2 < e1, 2, 1)

        def rule(idx, j, c0, c3=None):
            if not len(idx):
                return
            m = mod[j]
            l1, l2 = L1[idx, j], L2[idx, j]
            lin = (2 * l1 - l2) % m
            C = np.stack([c0 % m, l1, lin, lin if c3 is None else c3 % m])
            w = W[idx, j]
            u = (np.take_along_axis(C, w[None, :], 0)[0] + Rr[idx, j]) % m
            err = np.minimum((u[None, :] - C) % m, (C - u[None, :]) % m)
            U[idx, j] = u
            W[idx, j] = np.argmin(err, axis=0)

        def step(idx, role, c0, rule_on=True, c3=None):
            j = g[ROLEX[role]]
            if rule_on:
                rule(idx, j, c0, c3)
            else:
                generic(idx, np.array([j]))

        allr = np.arange(n)
        generic(allr, self.s1)
        ai = self.airix[U[:, g[ROLEX["TYPE"]]]]
        air = ai >= 0
        gr, ar = np.flatnonzero(~air), np.flatnonzero(air)
        generic(gr, self.s2)
        Lv = lambda L, r, idx=allr: L[idx, g[ROLEX[r]]]
        step(allr, "HEADING", L1[:, g[ROLEX["HEADING"]]] + U[:, g[ROLEX["TURN"]]])
        layer = U[:, g[ROLEX["LAYER"]]] & 3
        layer1 = L1[:, g[ROLEX["LAYER"]]] & 3
        if len(ar):
            a = ai[ar]
            acc, maxv, brake = self.a_acc[a], self.a_maxv[a], self.a_brake[a]
            airborne = layer[ar] == 2
            landing = (layer1[ar] == 2) & (layer[ar] == 1)
            s = lambda L, r: s32(Lv(L, r, ar))
            kx, _, kz = known(acc, maxv, brake, Lv(L1, "HEADING", ar), s(L1, "VX"), s(L1, "VY"), s(L1, "VZ"))
            qx, _, qz = known(acc, maxv, brake, Lv(L2, "HEADING", ar), s(L2, "VX"), s(L2, "VY"), s(L2, "VZ"))
            l3 = L3[ar]
            rx, _, rz = known(acc, maxv, brake, l3[:, 0], s32(l3[:, 2]), s32(l3[:, 3]), s32(l3[:, 4]))
            ok1 = (age[ar] >= 2) & (layer1[ar] == 2) & ((Lv(L2, "LAYER", ar) & 3) == 2)
            ok2 = ok1 & (age[ar] >= 3) & ((l3[:, 1] & 3) == 2)
            a1x = np.where(ok1, s32(s(L1, "VX") - qx), 0)
            a1z = np.where(ok1, s32(s(L1, "VZ") - qz), 0)
            a2x = np.where(ok2, s32(s(L2, "VX") - rx), 0)
            a2z = np.where(ok2, s32(s(L2, "VZ") - rz), 0)
            px, pz = rot(a1x, a1z, a2x, a2z)
            step(ar, "VX", np.where(airborne, s32(kx + a1x), 0), c3=np.where(airborne, s32(kx + px), 0))
            ax = s32(s32(Lv(U, "VX", ar)) - kx)
            root = isqrt(np.maximum(acc * acc - ax * ax, 0))
            vz = np.where(ax == a1x, s32(kz + a1z), s32(kz + np.where(a1z >= 0, root, -root)))
            step(ar, "VZ", np.where(airborne, vz, 0), c3=np.where(airborne, s32(kz + pz), 0))
            sp1, sp2 = s(L1, "SPEED"), s(L2, "SPEED")
            cl1 = np.where((sp1 & ~3) < 0x40000, 0x10000, sp1 >> 2)
            cl2 = np.where((sp2 & ~3) < 0x40000, 0x10000, sp2 >> 2)
            vy1 = s(L1, "VY")
            vy = np.where(vy1 == cl2, cl1, np.where(vy1 == -cl2, -cl1, 0))
            step(ar, "VY", np.where(airborne & (age[ar] >= 2), vy, 0))
            v = [np.clip(s32(Lv(U, r, ar)), -(1 << 30), 1 << 30) for r in ("VX", "VY", "VZ")]
            step(ar, "SPEED", np.where(airborne, isqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 0))
            for ra, rv in (("AX", "VX"), ("AY", "VY"), ("AZ", "VZ")):
                l1a = s(L1, ra)
                dec = mul_asr16(l1a, 0xF333)
                step(ar, ra, np.where(airborne, dec + s32(Lv(U, rv, ar)) - s(L1, rv),
                                      np.where(landing, dec, l1a)))
            uax, uaz, uh = s32(Lv(U, "AX", ar)), s32(Lv(U, "AZ", ar)), Lv(U, "HEADING", ar)
            on = airborne | landing
            step(ar, "BANK", np.where(on, bank_int(uax, uaz, uh, self.a_bank[a], self.q), Lv(L1, "BANK", ar)))
            step(ar, "ROLL", np.where(on, bank_int(uax, uaz, uh, self.a_pitch[a], self.q), Lv(L1, "ROLL", ar)))
        if len(gr):
            h, sp = Lv(U, "HEADING", gr), s32(Lv(U, "SPEED", gr))
            step(gr, "VX", -((SIN[((h + 0x20) >> 7) & 511] * sp + 0x1000) >> 13))
            step(gr, "VZ", -((SIN[((h + 0x4020) >> 7) & 511] * sp + 0x1000) >> 13))
        step(allr, "X", Lv(L1, "X") + s32(Lv(U, "VX")))
        step(allr, "Z", Lv(L1, "Z") + s32(Lv(U, "VZ")))
        if len(ar):
            step(ar, "Y", Lv(L1, "Y", ar) + s32(Lv(U, "VY", ar)))
        step(allr, "FX", (s32(Lv(U, "X")) - s16(Lv(U, "FW")) * 524288 + 0x80000) >> 20)
        step(allr, "FZ", (s32(Lv(U, "Z")) - s16(Lv(U, "FH")) * 524288 + 0x80000) >> 20)
        sight = bool(self.prm.sight)
        step(allr, "SX", s32(Lv(U, "X")) >> 21, rule_on=sight)
        moved = (Lv(U, "FX") != Lv(L1, "FX")) | (Lv(U, "FZ") != Lv(L1, "FZ")) | (layer != layer1)
        step(allr, "CLAIM", np.where(moved, (gt - 1) % (1 << 32), Lv(L1, "CLAIM")))
        moved = (Lv(U, "VX") != 0) | (Lv(U, "VY") != 0) | (Lv(U, "VZ") != 0) | (layer != layer1)
        step(allr, "STAMP", np.where(moved, (gt - 1) % (1 << 32), Lv(L1, "STAMP")))
        for j in self.counts:
            rule(allr, j, np.maximum(L1[:, j] - 1, 0))
        if len(ar):
            a = ai[ar]
            y = np.maximum(s32(Lv(U, "Y", ar)), (int(self.prm.sea) + 1) * 65536)
            hgt = np.clip(s16(y >> 16) + self.a_sight[a], 0, 255)
            step(ar, "SZ", ((s32(Lv(U, "Z", ar)) >> 16) - (hgt >> 1)) >> 5, rule_on=sight)
        # the end of the iteration for the evaluated candidates, and the deltas they make
        img = self.join(U)
        l2img_same = (L1 == L2).all(axis=1)
        same = (img == old).all(axis=1)
        ul3 = self._l3_of(U)
        still = (Lv(U, "VX") == 0) & (Lv(U, "VY") == 0) & (Lv(U, "VZ") == 0)
        rest = (age == 3) & same & l2img_same & (L3 == ul3).all(axis=1) & (still | (Lv(U, "STAMP") + 1 < gt))
        self.l3[rows] = self._l3_of(L2)
        self.l2[rows] = L1
        self.copy[rows] = img
        self.age[rows] = np.minimum(age + 1, 3)
        self.rest[rows] = rest
        self.win[rows] = W
        deltas = []
        ch = np.flatnonzero(~same)
        if len(ch):
            new32 = img[ch].view("<u4")
            old32 = old[ch].view("<u4")
            diff = new32 != old32
            bits = np.packbits(diff, axis=1, bitorder="little")
            for i, r in enumerate(ch.tolist()):
                s = int(rows[r])
                deltas.append(struct.pack("<H", s) + bits[i].tobytes() + b"\0\0\0" +
                              new32[i][diff[i]].astype("<u4").tobytes())
        return deltas


# ---- zstd and the block walk -------------------------------------------------------------------

WINDOW_MAX = 1 << 27                       # tagpu_reccodec.c's RECC_WINDOW_MAX


def unpack(body, raw):
    """One zstd frame of exactly `raw` bytes, no trailing byte, whose window is at most 2^27;
    Unreadable otherwise. The window is bounded by name from the frame's header, as the C reader
    bounds it: zstandard's decompressor would refuse a larger one by itself, the C one-shot
    decompression would not."""
    if not body or not raw:
        raise Unreadable("an empty packed block")
    try:
        import zstandard
    except ImportError:
        raise Unreadable("a packed block needs zstandard: the shared venv (CLAUDE.md, Python "
                         "tooling)") from None
    try:
        if zstandard.frame_content_size(body) != raw:
            raise Unreadable(f"a zstd frame whose content is not {raw} bytes")
        window = zstandard.get_frame_parameters(body).window_size
        if window > WINDOW_MAX:
            raise Unreadable(f"a zstd frame asking for a {window}-byte window, past 2^27")
        d = zstandard.ZstdDecompressor().decompressobj()
        out = d.decompress(body)
    except zstandard.ZstdError as e:
        raise Unreadable(f"a block that is not one zstd frame: {e}")
    if not d.eof or d.unused_data or len(out) != raw:
        raise Unreadable(f"a block that is not one zstd frame of {raw} bytes")
    return out


def entries(payload):
    out, at = [], 0
    while len(payload) - at >= 4:
        tag, n = struct.unpack_from("<HH", payload, at)
        if n > len(payload) - at - 4:
            raise Unreadable("a header entry cut short")
        out.append((tag, payload[at + 4:at + 4 + n]))
        at += (4 + n + 3) & ~3
    return out


Block = namedtuple("Block", "ordinal offset kind flags first last raw packed payload")


def header_slots(es):
    """The unit slots a header's entries name (RT_SLOTS), 0 when it names none."""
    slots = [struct.unpack("<I", b)[0] for t, b in es if t == tarec.RT_SLOTS and len(b) == 4]
    return slots[0] if slots else 0


def iter_blocks(path, decode=True, rest=True, before_stream=None, data=None, start=None):
    """Every complete block of a recording of format 8, in order: Block(ordinal (from 1), offset,
    kind, flags, first, last, raw, packed, payload) with a stream block's payload its records.
    With decode=False, stream, snapshot and minimap payloads are None. Returns (as a generator's
    value) why the reading stopped short, or None at a clean end; raises Unreadable at a block it
    refuses. `before_stream(ordinal, flags, bytes, frame)` sees each stream block's unpacked bytes
    and the frame it is about to be decoded with (the codec's differential test). `data` is the
    file's bytes when the caller has read them already (reccache.py hashes the very bytes it
    decodes); `path` then names them in the messages.

    `start` (offset, ordinal, root_slots) resumes a walk at an RB_HEADER block: the blocks after
    it are what a walk from the file's start yields, given the ordinal of the block before it and
    the slots of the file's first header (None before any). A header starts a new unit frame, so
    nothing else of the blocks before it reaches the blocks after it (reccache.py's pieces)."""
    if data is None:
        with open(path, "rb") as f:
            data = f.read()
    if data[:8] != tarec.REC_MAGIC:
        raise Unreadable(f"{path}: not a recording")
    at, ordinal, frame, root_slots = 8, 0, None, None
    if start is not None:
        at, ordinal, root_slots = start
    while at < len(data):
        head = data[at:at + 32]
        if len(head) < 32:
            return f"a block header cut short at byte {at}"
        magic, kind, flags, first, last, raw, packed, crc_body, crc_head = struct.unpack("<IHHIIIIII", head)
        if magic != tarec.REC_BLOCK_MAGIC or zlib.crc32(head[:28]) != crc_head:
            return f"a torn block header at byte {at}"
        if raw > PAYLOAD_MAX or packed > PAYLOAD_MAX:
            raise Unreadable(f"a block at byte {at} of {packed} bytes, {raw} unpacked: past what "
                             f"it may hold")
        body = data[at + 32:at + 32 + packed]
        if len(body) < packed:
            return f"a block cut short at byte {at}"
        if zlib.crc32(body) != crc_body:
            return f"a block failing its CRC at byte {at}"
        allowed = RBF_STORED | (RBF_SECTIONS | RBF_FRAME_RESET if kind == tarec.RB_STREAM else 0)
        if (flags & ~allowed or (flags & RBF_FRAME_RESET and not flags & RBF_SECTIONS) or
                (kind in (tarec.RB_HEADER, tarec.RB_END, tarec.RB_TIMELINES) and not flags & RBF_STORED)):
            raise Unreadable(f"block flags {flags:#x} on kind {kind} at byte {at}")
        if not flags & RBF_STORED and (not packed or not raw):
            raise Unreadable(f"a packed body of no bytes at byte {at}")
        if flags & RBF_STORED and raw != packed:
            raise Unreadable(f"a stored block of {packed} bytes, not {raw}, at byte {at}")
        ordinal += 1
        lazy = not decode and kind in (tarec.RB_STREAM, tarec.RB_SNAPSHOT, tarec.RB_MINIMAP)
        payload = None if lazy else body if flags & RBF_STORED else unpack(body, raw)
        try:
            if kind == tarec.RB_HEADER:
                es = entries(payload)
                fmt = [struct.unpack("<I", b)[0] for t, b in es if t == tarec.RT_FORMAT and len(b) == 4]
                if fmt != [REC_FORMAT]:
                    raise Unreadable(f"format {fmt[0] if fmt else None}, not {REC_FORMAT}")
                nslots = header_slots(es)
                if root_slots is not None and nslots != root_slots:
                    raise Unreadable(f"a segment of {nslots} slots after {root_slots}")
                root_slots = nslots
                # parsed only when one of the three is there, as tagpu_recreader.c's header_parse
                # does: a dump's header carries none of them and may name no slots
                if any(t in (RT_CODEC, RT_PHYSICS, RT_AIR) for t, _ in es):
                    prm = params(es, nslots)
                else:
                    prm = Params(nslots, [], 0, 0, 0, [], False)
                frame = Frame(prm, rest=rest)
            elif kind == tarec.RB_STREAM and payload is not None:
                if frame is None:
                    raise Unreadable("a stream block before any header")
                if before_stream:
                    before_stream(ordinal, flags, payload, frame)
                payload = frame.decode(flags, payload)
        except Unreadable as e:
            raise Unreadable(f"{e} (the block at byte {at})")
        yield Block(ordinal, at, kind, flags, first, last, raw, packed, payload)
        at += 32 + packed
    return None
