"""reccodec -- a recording's stream codec, read side: format 8's block walk, the sectioned
container and the unit frame, in numpy and numba.

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

INTEGERS ONLY: int64 scalars in the compiled kernels and int64 arrays around them, `>>` a floor
shift, `//` a floor division (equal to C's where both operands are not negative, which is
everywhere C divides), a lane's arithmetic modulo 2^(8 x its width) an `&` with the lane's mask
(the floor modulo of any int64), the same clamps as the C, so that no product reaches 2^63. The
sine table is computed from its formula at import, never read from the executable.

THE UNIT FRAME runs in numba kernels, a block's iterations in one call and each slot's lanes one
at a time: the frame is a few hundred small steps a slot, each too small for an array operation's
cost per call to pay for itself.

Needs numpy, numba for a sectioned block and zstandard for a packed block (the shared venv;
devpy.ensure() re-executes a tool under it). Each is imported where it is first needed, so a walk
with decode=False over a file whose only packed blocks are its stream, snapshots and minimap runs
with numpy alone.
"""
import importlib.util
import math
import os
import struct
import sys
import threading
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


_SECTION_OF = np.array([section_of(k) for k in range(1 << 16)], np.int64)


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


# ---- the kernels -------------------------------------------------------------------------------
#
# The functions marked @_jit or @_inline are compiled by numba on the first sectioned block
# (_compile), so a reading that decodes none never imports it. cache=True keeps their machine code
# in __pycache__ beside this file, keyed by this file's text: what a kernel reads from another
# module -- tarec's record kinds and image sizes -- is copied in when it compiles, and those change
# only with the format, which changes REC_FORMAT here too. The first decoding after an edit here
# compiles for about 8 s; a later one loads the cache in a fraction of a second.

_KERNELS = []
_compiled = False
_compile_lock = threading.Lock()


def _jit(f):
    _KERNELS.append((f.__name__, {}))
    return f


def _inline(f):
    """A kernel compiled into each caller: one called with constants (an error code, a rule's
    lane) is otherwise compiled once for each constant, 37 compilations of _fail and _rule
    (MEASURED 2026-10-08, the cache's files)."""
    _KERNELS.append((f.__name__, {"inline": "always"}))
    return f


def _compile():
    """Every kernel replaced by its numba dispatcher, once. Each compiles, or loads from the
    cache, at its first call, and by then finds the kernels it calls as dispatchers too.

    tareplay decodes from several threads of one process: the replacement runs under
    _compile_lock and _compiled turns true only after the last kernel is replaced, so a thread
    that skips the lock never finds a kernel half replaced, and none is decorated twice."""
    global _compiled
    if _compiled:
        return
    with _compile_lock:
        if _compiled:
            return
        try:
            import numba
        except ImportError:
            raise Unreadable("a sectioned block needs numba: the shared venv (CLAUDE.md, Python "
                             "tooling)") from None
        for name, options in _KERNELS:
            globals()[name] = numba.njit(cache=True, error_model="numpy",
                                         **options)(globals()[name])
        _compiled = True


# ---- integer arithmetic, one slot at a time (the C's operations; its bounds hold here) ----------
#
# Every value an int64 scalar: `//` and `%` floor as Python's do, `>>` is an arithmetic shift
# (numba keeps Python's semantics for negative operands), and a product past 2^63 would wrap as
# numpy's does -- the clamps keep every one below it. error_model="numpy" drops the zero-divisor
# test: every divisor here is a maximum with 1, the clamped maxv, u + 2^31 with u >= 0 or twice a
# norm > 0.

_SINC, _COSC, _ATC = (np.array(c, np.int64) for c in (SINC, COSC, ATC))


@_jit
def s32(a):
    return ((a + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


@_jit
def s16(a):
    return ((a + (1 << 15)) & 0xFFFF) - (1 << 15)


@_jit
def clip(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


@_jit
def mul_asr16(a, b):
    return s32((a * b) >> 16)


@_jit
def mulq(a, b):
    return (a * b) >> 31


@_jit
def isqrt(n):
    """floor(sqrt(n)) exactly for 0 <= n < 2^62: a float estimate, corrected once each way."""
    s = np.int64(math.floor(math.sqrt(np.float64(n))))
    if s * s > n:
        s -= 1
    if (s + 1) * (s + 1) <= n:
        s += 1
    return s


@_jit
def bitlen(a):
    n = 0
    while a > 0:
        a >>= 1
        n += 1
    return n


@_jit
def round24_sqrt(S):
    """sqrt(S) as (mantissa, exponent): 24 significant bits, rounded to nearest, ties to even."""
    k = max(26 - bitlen(isqrt(S)), 0)
    T = S << (2 * k)
    r = isqrt(T)
    sh = max(bitlen(r) - 24, 0)
    m = r >> sh
    if sh > 0:
        rem, half = r - (m << sh), np.int64(1) << (sh - 1)
        if rem > half or (rem == half and (r * r != T or m & 1)):
            m += 1
    if m >> 24:
        return m >> 1, sh + 1 - k
    return m, sh - k


@_jit
def table_mul(h, e, add):
    return s32((SIN[((h + add) >> 7) & 511] * e + 0x1000) >> 13)


@_jit
def known(acc, maxv, brake, h, vx, vz):
    d = max(0x10000 - (acc * 65536) // maxv, -(1 << 17))
    v1x = clip(mul_asr16(vx, d), -(1 << 30), 1 << 30)
    v1z = clip(mul_asr16(vz, d), -(1 << 30), 1 << 30)
    m, e = round24_sqrt(v1x * v1x + v1z * v1z)
    if e >= 0:
        if (m << e) <= brake:
            return v1x, v1z
        kf = s32((brake * 65536) // max(m << e, 1))
        ex = s32((m << e) - brake)
    else:
        if m <= (brake << -e):
            return v1x, v1z
        kf = s32(((brake * 65536) << -e) // max(m, 1))
        ex = s32((m >> -e) - brake)
    return (s32(mul_asr16(v1x, kf) - table_mul(h, ex, 0x20)),
            s32(mul_asr16(v1z, kf) - table_mul(h, ex, 0x4020)))


@_jit
def sin_cos_q31(h):
    h &= 0xFFFF
    n = ((h + 0x2000) >> 14) & 3
    x = ((((h + 0x2000) & 0x3FFF) - 0x2000) * TH47) >> 16
    x2 = mulq(x, x)
    s = _SINC[0]
    for i in range(1, len(_SINC)):
        s = mulq(s, x2) + _SINC[i]
    s = mulq(s, x)
    c = _COSC[0]
    for i in range(1, len(_COSC)):
        c = mulq(c, x2) + _COSC[i]
    if n == 0:
        return s, c
    if n == 1:
        return c, -s
    if n == 2:
        return -s, -c
    return -c, s


@_jit
def atan_q31(u):
    big = u > ATAN_BIG
    w = ((u - ONE) * ONE) // (u + ONE) if big else u
    w2 = mulq(w, w)
    a = _ATC[0]
    for i in range(1, len(_ATC)):
        a = mulq(a, w2) + _ATC[i]
    a = mulq(a, w)
    return PI4 + a if big else a


@_jit
def bank_int(ax, az, h, scale, q):
    ax = clip(ax, -(1 << 30), 1 << 30)
    az = clip(az, -(1 << 30), 1 << 30)
    if (h & 0xFFFF) == 0:
        xr = ax
    else:
        sn, cs = sin_cos_q31(h)
        xr = (ax * cs - az * sn + (1 << 30)) >> 31
    val = s32((-xr * scale) >> 16)
    a = abs(val)
    if a > q:
        ang = PI2 - atan_q31((q * ONE) // max(a, 1))
    else:
        ang = atan_q31((a * ONE) // max(q, 1))
    if val < 0:
        ang = -ang
    return (ang * K47 + (1 << 46)) >> 47


@_jit
def rot(x1, z1, x2, z2):
    x1, z1 = clip(x1, -(1 << 16), 1 << 16), clip(z1, -(1 << 16), 1 << 16)
    x2, z2 = clip(x2, -(1 << 16), 1 << 16), clip(z2, -(1 << 16), 1 << 16)
    n2 = x2 * x2 + z2 * z2
    if n2 <= 0 or (x1 == 0 and z1 == 0):
        return x1, z1
    dot = x1 * x2 + z1 * z2
    cr = x2 * z1 - z2 * x1
    return (2 * (x1 * dot - z1 * cr) + n2) // (2 * n2), (2 * (x1 * cr + z1 * dot) + n2) // (2 * n2)


# ---- varints ----------------------------------------------------------------------------------

@_jit
def _varint(buf, at, end):
    """One varint from buf[at:end] as the C reads it: (value, new at), the new at -1 when the
    varint is malformed or runs past `end`."""
    x = np.int64(0)
    for i in range(5):
        if at >= end:
            return x, -1
        b = np.int64(buf[at])
        at += 1
        if i == 4 and b > 0x0F:
            return x, -1
        x |= (b & 0x7F) << (7 * i)
        if b < 0x80:
            return x, at
    return x, -1


def _varints(buf, at, end, n):
    """n varints in a row from buf[at:end], each read as the C reads it; (values, new at) or
    None when one is malformed or runs past `end`."""
    if n == 0:
        return np.zeros(0, np.int64), at
    b = buf[at:end]
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


# ---- the container's sections -----------------------------------------------------------------
#
# A kernel that refuses writes a code and up to three numbers into its `err` and returns the code;
# _refuse words it. The codes are this file's own.

(E_RUN_SHORT, E_KIND, E_RUN_EMPTY, E_FIRST, E_RUN_PAST, E_BODY, E_BODY_LEFT, E_TICK, E_UNIT_LEN,
 E_UNIT_SLOT, E_NOT_CAND, E_DELTAS, E_PAST_PAYLOAD, E_PAYLOAD) = range(1, 15)
_WHY = {E_RUN_SHORT: "a kind run cut short", E_KIND: "record kind {0}",
        E_RUN_EMPTY: "a run of no records", E_FIRST: "the first record is not RK_TICK",
        E_RUN_PAST: "more records than the payload holds",
        E_BODY: "a body cut short or past 0xFFFF in section {0}",
        E_BODY_LEFT: "{1} bytes left in section {0}", E_TICK: "an RK_TICK of {0} bytes",
        E_UNIT_LEN: "a unit record of {0} bytes", E_UNIT_SLOT: "a unit record for slot {0}",
        E_NOT_CAND: "a listed slot that is not a candidate",
        E_DELTAS: "iteration {0}: the frame changes {1} units, its kinds hold {2} deltas",
        E_PAST_PAYLOAD: "the records pass the head's payload",
        E_PAYLOAD: "the records are {0} bytes, not the head's {1}"}


@_inline
def _fail(err, code, a=0, b=0, c=0):
    err[0], err[1], err[2], err[3] = code, a, b, c
    return code


def _refuse(err):
    raise Unreadable(_WHY[int(err[0])].format(*(int(v) for v in err[1:])))


@_jit
def _kind_runs(sec, at, end, payload, kinds, counts, err):
    """The kinds section's (kind, count) runs into kinds[] and counts[]: the number of runs and of
    RK_TICK records, or (-code, 0) refused."""
    n = records = ticks = 0
    while at < end:
        kind, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_RUN_SHORT), 0
        count, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_RUN_SHORT), 0
        if not 1 <= kind <= 0xFFFF:
            return -_fail(err, E_KIND, kind), 0
        if count < 1:
            return -_fail(err, E_RUN_EMPTY), 0
        if not records and kind != RK_TICK:
            return -_fail(err, E_FIRST), 0
        if count > payload // 4 - records:
            return -_fail(err, E_RUN_PAST), 0
        records += count
        if kind == RK_TICK:
            ticks += count
        kinds[n], counts[n] = kind, count
        n += 1
    return n, ticks


@_jit
def _bodies(sec, off, secs, rec_at, rec_ln, err):
    """Each body section's (length, bytes) entries, section by section, as many as its records:
    the record's body at rec_at[] and its length at rec_ln[]."""
    for s in range(RCS_PROJ, RCS_COUNT):
        at, end = off[s], off[s + 1]
        for i in range(len(secs)):
            if secs[i] != s:
                continue
            ln, at = _varint(sec, at, end)
            if at < 0 or ln > 0xFFFF or ln > end - at:
                return _fail(err, E_BODY, s)
            rec_at[i], rec_ln[i] = at, ln
            at += ln
        if at != end:
            return _fail(err, E_BODY_LEFT, s, end - at)
    return 0


# ---- the unit frame ---------------------------------------------------------------------------

# the lanes the rules name, by position in Frame.tab's J (Frame._derive)
_RULED = ("HEADING", "TURN", "SPEED", "VX", "VY", "VZ", "AX", "AY", "AZ", "X", "Y", "Z", "BANK",
          "ROLL", "FX", "FZ", "FW", "FH", "SX", "SZ", "CLAIM", "STAMP", "LAYER", "TYPE")
(R_HEADING, R_TURN, R_SPEED, R_VX, R_VY, R_VZ, R_AX, R_AY, R_AZ, R_X, R_Y, R_Z, R_BANK, R_ROLL,
 R_FX, R_FZ, R_FW, R_FH, R_SX, R_SZ, R_CLAIM, R_STAMP, R_LAYER, R_TYPE) = range(len(_RULED))
_L3 = np.array([R_HEADING, R_LAYER, R_VX, R_VY, R_VZ], np.int64)   # L3's five, in its order
# an aircraft's row of Frame.tab's `air`: RT_AIR's constants, clamped where they are used
A_MAXV, A_BRAKE, A_ACC, A_BANK, A_PITCH, A_SIGHT = range(6)


@_jit
def _err_of(u, c, mask):
    return min((u - c) & mask, (c - u) & mask)


@_jit
def _step(x, j, c0, c3, has3, rule):
    """Lane j of the slot in x, (L1, L2, Rr, U, W, the lane masks, J, L3) as _advance makes it:
    its value from the winner among its candidates plus its residual, into U, and the next winner,
    the first candidate nearest the value, into W. The candidates are c0, the previous value, the
    line through the last two and c3 (the line again without has3); rule=False is the generic
    step, which has only the previous value and the line."""
    L1, L2, Rr, U, W, lane_mask = x[0], x[1], x[2], x[3], x[4], x[5]
    m, l1 = lane_mask[j], L1[j]
    if not rule and l1 == L2[j] and Rr[j] == 0:
        # L1 == L2 makes the line the previous value; with no residual the value is that too, both
        # candidates are at 0 and the first, the previous value, wins: the arithmetic below, short
        U[j], W[j] = l1, 1
        return l1
    lin = (2 * l1 - L2[j]) & m
    w = W[j]
    if not rule:
        u = ((lin if w >= 2 else l1) + Rr[j]) & m
        U[j] = u
        W[j] = 2 if _err_of(u, lin, m) < _err_of(u, l1, m) else 1
        return u
    k0 = c0 & m
    k3 = c3 & m if has3 else lin
    u = ((k0 if w == 0 else l1 if w == 1 else lin if w == 2 else k3) + Rr[j]) & m
    U[j] = u
    best, win = _err_of(u, k0, m), 0
    e = _err_of(u, l1, m)
    if e < best:
        best, win = e, 1
    e = _err_of(u, lin, m)
    if e < best:
        best, win = e, 2
    if _err_of(u, k3, m) < best:
        win = 3
    W[j] = win
    return u


@_inline
def _rule(x, r, c0, c3=0, has3=False, rule=True):
    """_step on the lane of rule position r (_RULED)."""
    return _step(x, x[6][r], c0, c3, has3, rule)


@_inline
def _accel(x, ra, rv, airborne, landing):
    L1, U, J = x[0], x[3], x[6]
    l1a = s32(L1[J[ra]])
    dec = mul_asr16(l1a, 0xF333)
    _rule(x, ra, dec + s32(U[J[rv]]) - s32(L1[J[rv]]) if airborne else dec if landing else l1a)


@_jit
def _air_steps(x, row, age, layer, layer1, q):
    """An aircraft's velocity, speed, acceleration, bank and roll."""
    L1, L2, U, J, L3 = x[0], x[1], x[3], x[6], x[7]
    acc, maxv, brake = row[A_ACC], row[A_MAXV], row[A_BRAKE]
    airborne = layer == 2
    landing = layer1 == 2 and layer == 1
    kx, kz = known(acc, maxv, brake, L1[J[R_HEADING]], s32(L1[J[R_VX]]), s32(L1[J[R_VZ]]))
    qx, qz = known(acc, maxv, brake, L2[J[R_HEADING]], s32(L2[J[R_VX]]), s32(L2[J[R_VZ]]))
    rx, rz = known(acc, maxv, brake, L3[0], s32(L3[2]), s32(L3[4]))
    ok1 = age >= 2 and layer1 == 2 and (L2[J[R_LAYER]] & 3) == 2
    ok2 = ok1 and age >= 3 and (L3[1] & 3) == 2
    a1x = s32(s32(L1[J[R_VX]]) - qx) if ok1 else 0
    a1z = s32(s32(L1[J[R_VZ]]) - qz) if ok1 else 0
    a2x = s32(s32(L2[J[R_VX]]) - rx) if ok2 else 0
    a2z = s32(s32(L2[J[R_VZ]]) - rz) if ok2 else 0
    px, pz = rot(a1x, a1z, a2x, a2z)
    uvx = _rule(x, R_VX, s32(kx + a1x) if airborne else 0, s32(kx + px) if airborne else 0, True)
    ax = s32(s32(uvx) - kx)
    root = isqrt(max(acc * acc - ax * ax, 0))
    vz = s32(kz + a1z) if ax == a1x else s32(kz + (root if a1z >= 0 else -root))
    _rule(x, R_VZ, vz if airborne else 0, s32(kz + pz) if airborne else 0, True)
    sp1, sp2 = s32(L1[J[R_SPEED]]), s32(L2[J[R_SPEED]])
    cl1 = 0x10000 if (sp1 & ~3) < 0x40000 else sp1 >> 2
    cl2 = 0x10000 if (sp2 & ~3) < 0x40000 else sp2 >> 2
    vy1 = s32(L1[J[R_VY]])
    vy = cl1 if vy1 == cl2 else -cl1 if vy1 == -cl2 else 0
    _rule(x, R_VY, vy if airborne and age >= 2 else 0)
    v0 = clip(s32(U[J[R_VX]]), -(1 << 30), 1 << 30)
    v1 = clip(s32(U[J[R_VY]]), -(1 << 30), 1 << 30)
    v2 = clip(s32(U[J[R_VZ]]), -(1 << 30), 1 << 30)
    _rule(x, R_SPEED, isqrt(v0 * v0 + v1 * v1 + v2 * v2) if airborne else 0)
    _accel(x, R_AX, R_VX, airborne, landing)
    _accel(x, R_AY, R_VY, airborne, landing)
    _accel(x, R_AZ, R_VZ, airborne, landing)
    uax, uaz, uh = s32(U[J[R_AX]]), s32(U[J[R_AZ]]), U[J[R_HEADING]]
    on = airborne or landing
    _rule(x, R_BANK, bank_int(uax, uaz, uh, row[A_BANK], q) if on else L1[J[R_BANK]])
    _rule(x, R_ROLL, bank_int(uax, uaz, uh, row[A_PITCH], q) if on else L1[J[R_ROLL]])


@_jit
def _predict(x, tab, age, gt):
    """One slot's lanes into U, in the order where each rule reads only lanes it follows
    (tagpu_reccodec.c frame_slot): the generic lanes, the heading, the velocity (an aircraft's with
    its speed, acceleration and bank), the position, the cells, the stamps and the countdowns."""
    L1, U, J = x[0], x[3], x[6]
    s1, s2, counts, airix, air, consts = tab[5], tab[6], tab[7], tab[8], tab[9], tab[10]
    q, sea, sight = consts[0], consts[1], consts[2] != 0
    for j in s1:
        _step(x, j, 0, 0, False, False)
    a = airix[U[J[R_TYPE]]]
    if a < 0:
        for j in s2:
            _step(x, j, 0, 0, False, False)
    uh = _rule(x, R_HEADING, L1[J[R_HEADING]] + U[J[R_TURN]])
    layer, layer1 = U[J[R_LAYER]] & 3, L1[J[R_LAYER]] & 3
    if a >= 0:
        _air_steps(x, air[a], age, layer, layer1, q)
    else:
        sp = s32(U[J[R_SPEED]])
        _rule(x, R_VX, -((SIN[((uh + 0x20) >> 7) & 511] * sp + 0x1000) >> 13))
        _rule(x, R_VZ, -((SIN[((uh + 0x4020) >> 7) & 511] * sp + 0x1000) >> 13))
    ux = _rule(x, R_X, L1[J[R_X]] + s32(U[J[R_VX]]))
    uz = _rule(x, R_Z, L1[J[R_Z]] + s32(U[J[R_VZ]]))
    if a >= 0:
        _rule(x, R_Y, L1[J[R_Y]] + s32(U[J[R_VY]]))
    ufx = _rule(x, R_FX, (s32(ux) - s16(U[J[R_FW]]) * 524288 + 0x80000) >> 20)
    ufz = _rule(x, R_FZ, (s32(uz) - s16(U[J[R_FH]]) * 524288 + 0x80000) >> 20)
    _rule(x, R_SX, s32(ux) >> 21, 0, False, sight)
    moved = ufx != L1[J[R_FX]] or ufz != L1[J[R_FZ]] or layer != layer1
    _rule(x, R_CLAIM, (gt - 1) % (1 << 32) if moved else L1[J[R_CLAIM]])
    moved = U[J[R_VX]] != 0 or U[J[R_VY]] != 0 or U[J[R_VZ]] != 0 or layer != layer1
    _rule(x, R_STAMP, (gt - 1) % (1 << 32) if moved else L1[J[R_STAMP]])
    for j in counts:
        _step(x, j, max(L1[j] - 1, 0), 0, False, True)
    if a >= 0:
        y = max(s32(U[J[R_Y]]), (sea + 1) * 65536)
        hgt = clip(s16(y >> 16) + air[a, A_SIGHT], 0, 255)
        _rule(x, R_SZ, ((s32(uz) >> 16) - (hgt >> 1)) >> 5, 0, False, sight)


@_jit
def _lanes_of(img, off, width, out):
    for j in range(len(off)):
        v = np.int64(0)
        for k in range(width[j]):
            v |= np.int64(img[off[j] + k]) << (8 * k)
        out[j] = v


@_jit
def _split(copy, off, width):
    """Every slot's image cut into its lanes."""
    out = np.empty((copy.shape[0], len(off)), np.int64)
    for s in range(copy.shape[0]):
        _lanes_of(copy[s], off, width, out[s])
    return out


@_jit
def _advance(s, gt, tab, st, Rr, U, dirty, dbuf, at):
    """Slot s's iteration: its lanes predicted with the residuals Rr, its state moved on and,
    when its image changed, its RK_UNIT_DELTA body written at dbuf[at:]; the body's length, 0
    for an unchanged image. The image's bytes are written where a lane's value changed, and a
    dword is in the delta when one of its bytes did: the same dwords as comparing the images."""
    off, width, J = tab[0], tab[1], tab[3]
    copy, l1, l2, l3, win, age, rest = st
    L1, L2, L3, img = l1[s], l2[s], l3[s], copy[s]
    a = np.int64(age[s])
    _predict((L1, L2, Rr, U, win[s], tab[2], J, L3), tab, a, gt)
    resting = a == 3
    for i in range(5):
        resting &= L3[i] == U[J[_L3[i]]]
        L3[i] = L2[J[_L3[i]]]
    n = 0
    for j in range(len(off)):
        resting &= L1[j] == L2[j]
        if U[j] != L1[j]:
            for k in range(width[j]):
                b = off[j] + k
                v = (U[j] >> (8 * k)) & 0xFF
                if img[b] != v:
                    img[b] = v
                    if not dirty[b >> 2]:
                        dirty[b >> 2] = True
                        n += 1
        L2[j], L1[j] = L1[j], U[j]
    if n:
        # the slot, a bit for each changed dword, then those dwords
        dbuf[at], dbuf[at + 1] = s & 0xFF, s >> 8
        dbuf[at + 2:at + DELTA_HEAD] = 0
        v = at + DELTA_HEAD
        for d in range(NDW):
            if dirty[d]:
                dirty[d] = False
                dbuf[at + 2 + (d >> 3)] |= 1 << (d & 7)
                dbuf[v:v + 4] = img[4 * d:4 * d + 4]
                v += 4
    still = U[J[R_VX]] == 0 and U[J[R_VY]] == 0 and U[J[R_VZ]] == 0
    rest[s] = resting and n == 0 and (still or U[J[R_STAMP]] + 1 < gt)
    age[s] = min(a + 1, 3)
    return DELTA_HEAD + 4 * n if n else 0


@_jit
def _put(out, pos, kind, src, at, ln, err):
    """One record into out at pos, padded to a dword: the new pos, or -code past the payload."""
    rl = (4 + ln + 3) & ~3
    if rl > len(out) - pos:
        return -_fail(err, E_PAST_PAYLOAD)
    out[pos], out[pos + 1], out[pos + 2], out[pos + 3] = kind & 0xFF, kind >> 8, ln & 0xFF, ln >> 8
    out[pos + 4:pos + 4 + ln] = src[at:at + ln]
    return pos + rl


@_jit
def _iterations(sec, kinds, rec_at, rec_ln, listed, slots, R, tab, live, st, rest_mode, out, err):
    """The block's records into out, iteration by iteration: pass A takes the iteration's records
    and the slots its RK_UNIT and RK_UNIT_GONE name, the frame makes its deltas, pass B writes the
    records in order and the named slots restart from their images. 0, or the refusal's code."""
    off, width, J, defwin = tab[0], tab[1], tab[3], tab[4]
    copy, l1, l2, l3, win, age, rest = st
    nslots, nl, nrec = len(live), len(off), len(kinds)
    named = np.zeros(nslots, np.int64)          # it + 1 for the slots iteration `it` names
    touched = np.empty(nrec, np.int64)
    dbuf = np.empty(nslots * (DELTA_HEAD + IMG), np.uint8)
    dend = np.empty(nslots + 1, np.int64)
    Rr, U = np.empty(nl, np.int64), np.empty(nl, np.int64)
    dirty = np.zeros(NDW, np.bool_)
    pos = r = u0 = 0
    for it in range(len(listed)):
        # pass A
        k0, nt, ndelta, gt = r, 0, 0, 0
        while r < nrec and (r == k0 or kinds[r] != RK_TICK):
            kind, at, ln = kinds[r], rec_at[r], rec_ln[r]
            r += 1
            if kind == RK_UNIT_DELTA:
                ndelta += 1
            elif kind == RK_TICK:
                if ln != 4:
                    return _fail(err, E_TICK, ln)
                gt = (np.int64(sec[at]) | np.int64(sec[at + 1]) << 8 | np.int64(sec[at + 2]) << 16 |
                      np.int64(sec[at + 3]) << 24)
            elif kind == RK_UNIT or kind == RK_UNIT_GONE:
                if ln != (4 + IMG if kind == RK_UNIT else 4):
                    return _fail(err, E_UNIT_LEN, ln)
                s = np.int64(sec[at]) | np.int64(sec[at + 1]) << 8
                if not 1 <= s < nslots:
                    return _fail(err, E_UNIT_SLOT, s)
                touched[nt] = s
                nt += 1
                named[s] = it + 1
        # the frame: every candidate -- live, not slot 0, not named -- but a resting one unless
        # listed, in slot order, the listed ones in theirs
        c = listed[it]
        for p in range(c):
            s = slots[u0 + p]
            if not live[s] or named[s] == it + 1:
                return _fail(err, E_NOT_CAND)
        nd, p = 0, 0
        dend[0] = 0
        for s in range(1, nslots):
            if not live[s] or named[s] == it + 1:
                continue
            if p < c and slots[u0 + p] == s:
                Rr[:] = R[u0 + p]
                p += 1
            elif rest_mode and rest[s]:
                continue
            else:
                Rr[:] = 0
            n = _advance(s, gt, tab, st, Rr, U, dirty, dbuf, dend[nd])
            if n:
                dend[nd + 1] = dend[nd] + n
                nd += 1
        u0 += c
        if nd != ndelta:
            return _fail(err, E_DELTAS, it, nd, ndelta)
        # pass B
        d = 0
        for q in range(k0, r):
            kind = kinds[q]
            if kind == RK_UNIT_DELTA:
                pos = _put(out, pos, kind, dbuf, dend[d], dend[d + 1] - dend[d], err)
                d += 1
            else:
                at, ln = rec_at[q], rec_ln[q]
                if kind == RK_UNIT or kind == RK_UNIT_GONE:
                    s = np.int64(sec[at]) | np.int64(sec[at + 1]) << 8
                    live[s] = kind == RK_UNIT
                    if kind == RK_UNIT:
                        copy[s, :] = sec[at + 4:at + 4 + IMG]
                pos = _put(out, pos, kind, sec, at, ln, err)
            if pos < 0:
                return err[0]
        # the slots the records named restart from their images
        for i in range(nt):
            s = touched[i]
            if live[s]:
                _lanes_of(copy[s], off, width, l1[s])
                l2[s, :] = l1[s]
                for k in range(5):
                    l3[s, k] = l1[s, J[_L3[k]]]
                win[s, :] = defwin
                age[s] = 1
            else:
                age[s] = 0
            rest[s] = False
    if pos != len(out):
        return _fail(err, E_PAYLOAD, pos, len(out))
    return 0


class Frame:
    """One reading's unit frame: the copy of the units and its lanes (L1), L2, L3's five lanes,
    the winners, the history's age and the resting flags, for a segment's params. l1 is the copy
    cut into lanes for every live slot between iterations: a restart and a slot's RK_UNIT cut it
    again, and an advance moves both on together. `rest=False` evaluates every candidate every
    iteration, which must give the same bytes (the fixed-point argument)."""

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
        for k in ("copy", "live", "l1", "l2", "l3", "win", "age", "rest"):
            if hasattr(self, k):
                setattr(f, k, getattr(self, k).copy())
        return f

    def _derive(self):
        """The segment's lane table and constants as the kernels take them, `tab`: off, width,
        the lane masks (2^(8 width) - 1), J (the lane of each _RULED role), the default winners,
        S1's and S2's lanes, the COUNT lanes, the aircraft index by type, the aircraft rows and
        (q, sea, sight)."""
        p = self.prm
        self.nl = len(p.lanes)
        if not self.nl:
            return
        off = np.array([o for o, _, _ in p.lanes], np.int64)
        width = np.array([w for _, w, _ in p.lanes], np.int64)
        role = [r for _, _, r in p.lanes]
        lane = {r: j for j, r in enumerate(role) if r not in (ROLE["PLAIN"], ROLE["COUNT"])}
        J = np.array([lane[ROLE[name]] for name in _RULED], np.int64)
        lanes_of = lambda group: np.array([j for j, r in enumerate(role) if group(r)], np.int64)
        defwin = np.array([1 if r in _S1 else 0 for r in role], np.int8)
        q = (min(max(p.gravity, 0), 1 << 24) * 65536) // 0xCCD
        airix = np.full(65536, -1, np.int64)
        for i, a in enumerate(p.air):
            airix[a.type] = i
        cl = lambda v, lo, hi: min(max(v, lo), hi)
        big = 1 << 24
        air = np.array([(cl(a.maxv, 1, big), cl(a.brake, 0, big), cl(a.acc, 0, big),
                         cl(a.bank, -big, big), cl(a.pitch, -big, big), a.sight_h)
                        for a in p.air] or [(1, 0, 0, 0, 0, 0)], np.int64)
        self.tab = (off, width, (np.int64(1) << (8 * width)) - 1, J, defwin,
                    lanes_of(lambda r: r in _S1), lanes_of(lambda r: r in _S2),
                    lanes_of(lambda r: r == ROLE["COUNT"]), airix, air,
                    np.array([q, p.sea, p.sight], np.int64))
        self.l3_lanes = J[_L3]

    def restart(self, prm=None):
        """The frame restarts from this reading's own copy of the units."""
        _compile()
        if prm is not None:
            self.prm = prm
            self._derive()
        self.valid = False
        if not self.prm.usable or not self.nl:
            return
        n = self.prm.nslots
        self.copy[~self.live] = 0
        self.l1 = _split(self.copy, self.tab[0], self.tab[1])
        self.l2 = self.l1.copy()
        self.l3 = self.l1[:, self.l3_lanes]
        self.win = np.tile(self.tab[4], (n, 1))
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
        out = self._decode(np.frombuffer(body, np.uint8))
        self.valid = True
        return out

    # -- the container --------------------------------------------------------------------------
    def _decode(self, sec):
        _compile()
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
        err = np.zeros(4, np.int64)
        # the kinds
        at, end = off[RCS_KINDS], off[RCS_KINDS + 1]
        run_kind, run_count = np.empty((2, (end - at) // 2), np.int64)
        runs, ticks = _kind_runs(sec, at, end, payload, run_kind, run_count, err)
        if runs < 0:
            _refuse(err)
        if ticks != iters:
            raise Unreadable(f"{ticks} RK_TICK records, not {iters} iterations")
        kinds = np.repeat(run_kind[:runs], run_count[:runs])
        # the bodies: each section's (length, bytes) entries in order, as many as its kinds hold
        secs = _SECTION_OF[kinds]
        rec_at, rec_ln = np.zeros((2, len(kinds)), np.int64)
        if _bodies(sec, np.array(off, np.int64), secs, rec_at, rec_ln, err):
            _refuse(err)
        listed, slots, R = self._units_parse(sec, off[RCS_UNITS], off[RCS_UNITS + 1], iters)
        out = np.zeros(payload, np.uint8)
        st = (self.copy, self.l1, self.l2, self.l3, self.win, self.age, self.rest)
        if _iterations(sec, kinds, rec_at, rec_ln, listed, slots, R, self.tab, self.live, st,
                       self.rest_mode, out, err):
            _refuse(err)
        return out.tobytes()

    def _units_parse(self, sec, at, end, iters):
        """The unit frame's section: how many slots each iteration lists, those slots in one array
        and their residuals by lane."""
        nl, nslots = self.nl, self.prm.nslots
        mb = (nl + 7) // 8
        v, at = _varint(sec, at, end)
        if at < 0 or v != iters:
            raise Unreadable("the unit frame's iterations are not the head's")
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
        # each iteration's slots climb from 0 by gap + 1: an iteration's last is its greatest
        steps = np.cumsum(gaps + 1)
        first = np.cumsum(listed) - listed
        slots = steps - np.repeat(np.concatenate(([0], steps))[first], listed)
        if total and (slots >= nslots).any():
            raise Unreadable("a listed slot past the slots")
        nd, at = _varint(sec, at, end)
        if at < 0:
            raise Unreadable("the mask count cut short")
        if (not 1 <= nd <= total) if total else nd != 0:
            raise Unreadable(f"{nd} masks for {total} listed units")
        if nd * mb > end - at:
            raise Unreadable("the masks run past the unit frame")
        dic = sec[at:at + nd * mb].reshape(nd, mb)
        at += nd * mb
        if nd and (~dic.any(axis=1)).any():
            raise Unreadable("an empty mask")
        if nd and nl % 8 and (dic[:, mb - 1] >> (nl % 8)).any():
            raise Unreadable("a mask names a lane past the table")
        if nd <= 256:
            if total > end - at:
                raise Unreadable("the mask ids cut short")
            ids = sec[at:at + total].astype(np.int64)
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
        width, lane_mask = self.tab[1], self.tab[2]
        c0 = 0
        for j in range(nl):
            k = int(cnt[j])
            if not k:
                continue
            z = vals[c0:c0 + k]
            c0 += k
            if width[j] < 4 and (z >> (8 * int(width[j]))).any():
                raise Unreadable(f"lane {j}'s residual is past its width")
            R[M[:, j], j] = ((z >> 1) ^ -(z & 1)) & lane_mask[j]
        return listed, slots, R


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
