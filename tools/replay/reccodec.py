"""reccodec -- a recording's stream codec, read side: format 13's block walk, the sectioned
container, the unit frame, the pool frame and the four record frames, in numpy and numba.

    import reccodec
    for blk in reccodec.iter_blocks(path):     # every complete block, stream payloads decoded
        ...

THE FORMAT is tagpu/ddraw/inc/tagpu_recfmt.h (RBF_SECTIONS, RecSecHead, RCS_*, RT_CODEC, RT_PHYSICS,
RT_AIR) and tagpu/ddraw/inc/tagpu_recframe.h, and the codec tagpu/ddraw/src/tagpu_reccodec.c with
its record frames tagpu/ddraw/src/tagpu_recframe_*.c, which the writer and the DLL's reader run.
This file is an INDEPENDENT implementation of its decoder, not a port to be kept in step line by
line: tagpu/ddraw/tests/codectest.py decodes every block of a recording with both and requires the
same bytes, and feeds both the same mutated blocks and requires the same accept or refuse. The
rules, their engine facts and their bounds are written down beside the C; here only what differs
in the doing.

A stream block with RBF_SECTIONS decodes to the records it was made from; one without is the
records themselves. RBF_REFRESH, on a stream block alone, says its last iteration is a refresh
(tagpu_recwriter.c, iteration_done) and changes nothing of the decoding. An RB_EXACT block, the
exact records of the stream block before it (the test lever tagpu_rec.exact), is records, never
sectioned, and no frame reads it. The frames' state crosses blocks inside a run of sectioned blocks and
restarts at RBF_FRAME_RESET -- the unit and pool frames from this reader's own copies of the units
and the projectile pool, the cells frame from this reading's own grid (every image it has read, a
records block's too), the other record frames empty -- which is why the walk is in order and per
segment: a block that continues a frame this reading did not start is refused.

INTEGERS ONLY: int64 scalars in the compiled kernels and int64 arrays around them, `>>` a floor
shift, `//` a floor division (equal to C's where both operands are not negative, which is
everywhere C divides), a lane's arithmetic modulo 2^(8 x its width) an `&` with the lane's mask
(the floor modulo of any int64), the same clamps as the C, so that no product reaches 2^63. The
sine table is computed from its formula at import, never read from the executable.

THE UNIT FRAME runs in numba kernels, a block's iterations in one call and each slot's lanes one
at a time: the frame is a few hundred small steps a slot, each too small for an array operation's
cost per call to pay for itself. THE POOL FRAME's control values are bounded by a kernel, its
codes and columns read as one array of varints, and each iteration decoded by a kernel that
makes every projectile record's body from the pool before and after it; it runs inside the
iterations' kernel, because an iteration's births refer to the last iteration's shots, whose
bodies the events frame makes there. THE RECORD FRAMES each have a region of this file (THE CELLS
FRAME, THE ORDERS FRAME, THE EVENTS FRAME, THE STATE FRAME): a state of numpy arrays and the
kernels the iterations' kernel calls, one region per frame file of the C.

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

REC_FORMAT = 13
RBF_STORED, RBF_SECTIONS, RBF_FRAME_RESET, RBF_REFRESH = 1, 2, 4, 8
RT_CODEC, RT_PHYSICS, RT_AIR = 29, 30, 31
RT_CODEC_DECLINED, RT_CODEC_MISMATCH = 44, 45
(RCS_KINDS, RCS_UNITS, RCS_PROJ, RCS_CELLS, RCS_ORDERS, RCS_EVENTS, RCS_OTHER, RCS_STATE) = range(8)
RCS_COUNT = 8
SEC_HEAD = 8 + 4 * RCS_COUNT               # RecSecHead: 40 bytes
LANES_MAX, COUNTS_MAX, AIR_MAX, TICKS_MAX = 128, 8, 1024, 256
GRID_MAX, WRECKS_MAX = 1 << 22, 65536      # RECC_GRID_MAX, RECC_WRECKS_MAX
FRAME_CAP = 16 << 20
PAYLOAD_MAX = 64 << 20                     # RECR_BLOCK_MAX
IMG = tarec.REC_UNIT_IMAGE                 # 0x148
NDW = IMG // 4
DELTA_HEAD = tarec.REC_UNIT_DELTA_HEAD     # 16
RK_TICK, RK_UNIT, RK_UNIT_DELTA, RK_UNIT_GONE = tarec.RK_TICK, tarec.RK_UNIT, tarec.RK_UNIT_DELTA, tarec.RK_UNIT_GONE
RK_PROJ_HEAD, RK_PROJ, RK_PROJ_DELTA = tarec.RK_PROJ_HEAD, tarec.RK_PROJ, tarec.RK_PROJ_DELTA
RK_EV_SHOT = tarec.RK_EV_SHOT
PDW = 27                                   # a projectile image's dwords (REC_PROJ_DWORDS)
PIMG = 4 * PDW
_POOL_BODY_MAX = tarec.REC_PROJ_DELTA_HEAD + PIMG  # a delta of all 27 dwords: 116 bytes
SHOT = 36                                  # an RK_EV_SHOT's body
SHOTS_MAX, WEAPON_IDS = 4096, 4096         # RECC_SHOTS_MAX, RECC_WEAPON_IDS
M32 = 0xFFFFFFFF

SIN = np.array([round(8192 * math.sin(2 * math.pi * i / 512)) for i in range(512)], np.int64)
ONE = 1 << 31
TH47, PI4, PI2, K47, ATAN_BIG = 13493037705, 1686629713, 3373259426, 683565276, 889516847
SINC = [0, 0, -54, 5918, -426088, 17895697, -357913941, 2147483648]
COSC = [0, 4, -592, 53261, -2982616, 89478485, -1073741824, 2147483648]
ATC = [74051160, -79536431, 85899346, -93368854, 102261126, -113025455, 126322568, -143165577,
       165191050, -195225786, 238609294, -306783378, 429496730, -715827883, 2147483648]

# the steps' lane groups, by role (tagpu_reccodec.c frame_slot)
_S1 = {ROLE[r] for r in ("PLAIN", "TURN", "FW", "FH", "LAYER", "TYPE", "OWNER")}
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
    if kind in (tarec.RK_GLOBALS, tarec.RK_GLOBALS_DELTA, tarec.RK_PLAYER, tarec.RK_PLAYER_DELTA,
                tarec.RK_WRECK, tarec.RK_FEATURES):
        return RCS_STATE
    return RCS_OTHER


_SECTION_OF = np.array([section_of(k) for k in range(1 << 16)], np.int64)


# ---- the header's entries ---------------------------------------------------------------------

Params = namedtuple("Params", "nslots nproj gw gh nwrecks lanes gravity sea sight air usable")
AirRow = namedtuple("AirRow", "type sight_h maxv brake acc bank pitch")


def sizes_ok(nslots, nproj, gw, gh, nwrecks):
    """The sizes a codec is made for, each within its bound (tagpu_reccodec.c sizes_ok)."""
    return (2 <= nslots <= 0xFFFF and 1 <= nproj <= 0xFFFF and 1 <= gw <= 0xFFFF and
            1 <= gh <= 0xFFFF and gw * gh <= GRID_MAX and 1 <= nwrecks <= WRECKS_MAX)


def params(entries, nslots, nproj, gw, gh, nwrecks):
    """A segment's codec constants from its header's (tag, body) entries, RT_SLOTS,
    RT_PROJECTILES and RT_FEATURES' grid and wreck pool; Unreadable when one is malformed, past a
    bound or given twice. `usable`: RT_CODEC and RT_PHYSICS are both there and the grid and the
    wreck pool are within the codec's bounds -- past them, the codec is off, not the header
    unreadable."""
    got = {}
    for tag, body in entries:
        if tag in (RT_CODEC, RT_PHYSICS, RT_AIR):
            if tag in got:
                raise Unreadable(f"header tag {tag} twice")
            got[tag] = bytes(body)
    if not 2 <= nslots <= 0xFFFF:
        raise Unreadable(f"RT_SLOTS {nslots} is outside 2..65535")
    if not 1 <= nproj <= 0xFFFF:
        raise Unreadable(f"RT_PROJECTILES {nproj} is outside 1..65535")
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
        missing = [ROLES[r][0] for r in range(1, NROLES) if r != ROLE["COUNT"] and r not in seen]
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
    usable = RT_CODEC in got and RT_PHYSICS in got and sizes_ok(nslots, nproj, gw, gh, nwrecks)
    return Params(nslots, nproj, gw, gh, nwrecks, lanes, gravity, sea, sight, air, usable)


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
# _refuse words it. The codes are this file's own: the core's below 100, then each record frame's
# in its region -- the cells 100-149, the orders 150-199, the events 200-249, the state 250-299.

(E_RUN_SHORT, E_KIND, E_RUN_EMPTY, E_FIRST, E_RUN_PAST, E_BODY, E_BODY_LEFT, E_TICK, E_UNIT_LEN,
 E_UNIT_SLOT, E_NOT_CAND, E_DELTAS, E_PAST_PAYLOAD, E_PAYLOAD, E_P_ITERS, E_P_CUT, E_P_COUNT,
 E_P_EXTENT, E_P_OVER, E_P_DEADS, E_P_DEAD, E_P_SURV, E_P_TAILS, E_P_TAIL, E_P_REF, E_P_HEADS,
 E_P_NOHEAD, E_P_SLOTS, E_F_SCHEME, E_F_NCOLS, E_F_TABLE, E_F_PAST, E_F_FILL, E_F_LEFT,
 E_F_BIG) = range(1, 36)
_WHY = {E_RUN_SHORT: "a kind run cut short", E_KIND: "record kind {0}",
        E_RUN_EMPTY: "a run of no records", E_FIRST: "the first record is not RK_TICK",
        E_RUN_PAST: "more records than the payload holds",
        E_BODY: "a body cut short or past 0xFFFF in section {0}",
        E_BODY_LEFT: "{1} bytes left in section {0}", E_TICK: "an RK_TICK of {0} bytes",
        E_UNIT_LEN: "a unit record of {0} bytes", E_UNIT_SLOT: "a unit record for slot {0}",
        E_NOT_CAND: "a listed slot that is not a candidate",
        E_DELTAS: "iteration {0}: the frame changes {1} units, its kinds hold {2} deltas",
        E_PAST_PAYLOAD: "the records pass the head's payload",
        E_PAYLOAD: "the records are {0} bytes, not the head's {1}",
        E_P_ITERS: "the pool frame's iterations are not the head's {0}",
        E_P_CUT: "iteration {0}'s pool frame control values cut short",
        E_P_COUNT: "iteration {0}'s count {1} is outside 0..{2}",
        E_P_EXTENT: "iteration {0}'s extent {1} is past the pool's {2}",
        E_P_OVER: "iteration {0}'s count {1} is past its extent {2}",
        E_P_DEADS: "iteration {0}: {1} deaths among {2} records",
        E_P_DEAD: "iteration {0}: a dead slot {1} past the {2} records",
        E_P_SURV: "iteration {0}: {1} survivors past its count {2}",
        E_P_TAILS: "iteration {0}: {1} tail slots past the count",
        E_P_TAIL: "iteration {0}: a tail slot {1} past the extent {2}",
        E_P_REF: "iteration {0}: a reference {1} past section {2}",
        E_P_HEADS: "iteration {0} has {1} RK_PROJ_HEAD",
        E_P_NOHEAD: "iteration {0} changes the pool's count or extent with no RK_PROJ_HEAD",
        E_P_SLOTS: "iteration {0}: the pool frame changes {1} records, its kinds hold {2}",
        E_F_SCHEME: "section {0}'s scheme {1}, this reader takes {2}",
        E_F_NCOLS: "section {0}'s column count is not its {1}",
        E_F_TABLE: "section {0}'s column table cut short",
        E_F_PAST: "section {0}'s column {1} runs past it",
        E_F_FILL: "{1} bytes of section {0} follow its columns",
        E_F_LEFT: "{2} bytes left in section {0}'s column {1}",
        E_F_BIG: "section {0} made a body of {1} bytes"}


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
    """RCS_OTHER's (length, bytes) entries, as many as its records: the record's body at rec_at[]
    and its length at rec_ln[]. The frames make the rest."""
    at, end = off[RCS_OTHER], off[RCS_OTHER + 1]
    for i in range(len(secs)):
        if secs[i] != RCS_OTHER:
            continue
        ln, at = _varint(sec, at, end)
        if at < 0 or ln > 0xFFFF or ln > end - at:
            return _fail(err, E_BODY, RCS_OTHER)
        rec_at[i], rec_ln[i] = at, ln
        at += ln
    if at != end:
        return _fail(err, E_BODY_LEFT, RCS_OTHER, end - at)
    return 0


# ---- the unit frame ---------------------------------------------------------------------------

# the lanes the rules name, by position in Frame.tab's J (Frame._derive)
_RULED = ("HEADING", "TURN", "SPEED", "VX", "VY", "VZ", "AX", "AY", "AZ", "X", "Y", "Z", "BANK",
          "ROLL", "FX", "FZ", "FW", "FH", "SX", "SZ", "CLAIM", "STAMP", "LAYER", "TYPE")
(R_HEADING, R_TURN, R_SPEED, R_VX, R_VY, R_VZ, R_AX, R_AY, R_AZ, R_X, R_Y, R_Z, R_BANK, R_ROLL,
 R_FX, R_FZ, R_FW, R_FH, R_SX, R_SZ, R_CLAIM, R_STAMP, R_LAYER, R_TYPE) = range(len(_RULED))
_L3 = np.array([R_HEADING, R_LAYER, R_VX, R_VY, R_VZ], np.int64)   # L3's five, in its order
_LEDGER = ("OWNER", "EREQ", "EGRANT", "MREQ", "MGRANT")      # Frame.tab's JL, in this order
OWNERS = 16                             # RECC_OWNERS: the owner IDs the ledger keeps a settle for
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


@_jit
def f32_add(a, b):
    """a + b as IEEE singles, rounded to nearest even, in integers (tagpu_reccodec.c
    reccodec_f32_add); an infinity or a NaN gives a."""
    sa, sb, ea, eb = a >> 31, b >> 31, (a >> 23) & 0xFF, (b >> 23) & 0xFF
    ma, mb = a & 0x7FFFFF, b & 0x7FFFFF
    if ea == 0xFF or eb == 0xFF:
        return a
    if ea:
        ma |= 0x800000
    else:
        ea = 1
    if eb:
        mb |= 0x800000
    else:
        eb = 1
    ma <<= 3
    mb <<= 3
    if ea < eb or (ea == eb and ma < mb):
        sa, sb, ea, eb, ma, mb = sb, sa, eb, ea, mb, ma
    t = ea - eb
    if t > 27:
        mb = 1 if mb else 0
    elif t:
        mb = (mb >> t) | (1 if mb & ((1 << t) - 1) else 0)
    if sa == sb:
        m = ma + mb
        if m >> 27:
            m = (m >> 1) | (m & 1)
            ea += 1
    else:
        m = ma - mb
        if not m:
            return 0
        while m < (1 << 26) and ea > 1:
            m <<= 1
            ea -= 1
    low = m & 7
    m >>= 3
    if low > 4 or (low == 4 and (m & 1)):
        m += 1
    if m >> 24:
        m >>= 1
        ea += 1
    if ea >= 0xFF:
        return (sa << 31) | 0x7F800000
    return (sa << 31) | ((ea if m >> 23 else 0) << 23) | (m & 0x7FFFFF)


@_jit
def _ledger(x, JL, step, lset, gt):
    """The ledger's requested and granted, energy then metal (tagpu_reccodec.c ledger_steps):
    requested 0 at its owner's settle, L1 + the step while it grows, L1 when it held and the step
    after a zero that followed a value; granted this tick's requested when the two were equal,
    else L1; every winner the rule. step is the slot's two steps, lset (settle, seen) per owner."""
    L1, L2, U, W = x[0], x[1], x[3], x[4]
    own = U[JL[0]]
    settle = own < OWNERS and lset[own, 1] != 0 and ((gt - lset[own, 0]) & 0xFFFFFFFF) % 30 == 0
    for k in range(2):
        jr, jg = JL[1 + 2 * k], JL[2 + 2 * k]
        l1, l2, g1 = L1[jr], L2[jr], L1[jg]
        if l1 and settle:
            p = 0
        elif not l1:
            p = step[k] if l2 else 0
        elif l1 == l2:
            p = l1
        else:
            p = f32_add(l1, step[k])
        W[jr] = 0
        ur = _step(x, jr, p, 0, False, True)
        W[jr] = 0
        if not l1 and ur:
            step[k] = ur
        if l1 and not ur and own < OWNERS:
            lset[own, 0], lset[own, 1] = gt, 1
        W[jg] = 0
        _step(x, jg, 0 if not ur else ur if g1 == l1 else g1, 0, False, True)
        W[jg] = 0


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
    off, width, J, JL = tab[0], tab[1], tab[3], tab[11]
    copy, l1, l2, l3, win, age, rest, lstep, lset = st
    L1, L2, L3, img = l1[s], l2[s], l3[s], copy[s]
    a = np.int64(age[s])
    x = (L1, L2, Rr, U, win[s], tab[2], J, L3)
    _predict(x, tab, a, gt)
    _ledger(x, JL, lstep[s], lset, gt)
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
    ledger = U[JL[1]] != 0 or U[JL[2]] != 0 or U[JL[3]] != 0 or U[JL[4]] != 0
    rest[s] = resting and n == 0 and (still or U[J[R_STAMP]] + 1 < gt) and not ledger
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
def _iterations(sec, kinds, secs, rec_at, rec_ln, listed, slots, R, tab, live, st, rest_mode, pl,
                fst, foff, out, err):
    """The block's records into out, iteration by iteration: the pool frame's iteration
    (_pool_iter), pass A takes the iteration's records and the slots its RK_UNIT and RK_UNIT_GONE
    name, the unit frame makes its deltas, pass B writes the records in order -- a projectile
    record's body from the pool frame's, a record frame's from its frame at the unit copy and the
    events flag the encoder's walk had there (tagpu_reccodec.c frames_walk) -- and the named slots
    restart from their images. pl is the pool frame's parsed section and state (Frame._pool), fst
    the record frames' states and foff the sections' offsets. 0, or the refusal's code."""
    off, width, J, defwin = tab[0], tab[1], tab[3], tab[4]
    copy, l1, l2, l3, win, age, rest, lstep, lset = st
    ctl, codes, S, B, T, P, H, pwin, page, lastnew, shots, meta, payload = pl
    nslots, nl, nrec, iters = len(live), len(off), len(kinds), len(listed)
    # the record frames' sections, read and bounded before the first iteration
    fcols = np.zeros((4, FRAME_COLS_MAX, 2), np.int64)
    c = _cells_open(fst[0], fcols[0], sec, foff[RCS_CELLS], foff[RCS_CELLS + 1], iters, err)
    if c == 0:
        c = _orders_open(fst[1], fcols[1], sec, foff[RCS_ORDERS], foff[RCS_ORDERS + 1], iters, err)
    if c == 0:
        c = _events_open(fst[2], fcols[2], sec, foff[RCS_EVENTS], foff[RCS_EVENTS + 1], iters, err)
    if c == 0:
        c = _state_open(fst[3], fcols[3], sec, foff[RCS_STATE], foff[RCS_STATE + 1], iters, err)
    if c:
        return c
    # the pool frame's scratch: the pool an iteration makes, and what it builds on the way. The
    # pool and its scratch swap each iteration; the state ends in the caller's arrays.
    n = P.shape[0]
    Pn, Hn = np.zeros_like(P), np.empty_like(H)
    winn, agen = np.zeros_like(pwin), np.zeros_like(page)
    scr = (np.empty_like(P), np.zeros(n, np.bool_), np.empty((SHOTS_MAX, PDW), np.int64),
           np.empty(n, np.bool_), np.empty(n, np.int64), np.empty(n, np.int64),
           np.empty(n, np.int64), np.empty(n, np.int64), np.zeros(PDW, np.int64),
           np.empty(PDW, np.int64), np.empty(3, np.int64), np.empty(PDW, np.int8),
           np.empty(4, np.int64), np.empty(4, np.int64))
    pcur = np.zeros(6, np.int64)
    # _pool_iter writes a body before it tests the payload's bound, with its end at most the
    # payload: the slack holds the longest body, a delta of all 27 dwords (8 + 108 bytes), so
    # numba's unchecked stores stay in the array whatever the file says
    pbuf = np.zeros(payload + _POOL_BODY_MAX, np.uint8)
    p_at, p_ln = np.zeros(nrec, np.int64), np.zeros(nrec, np.int64)
    flip = False
    body = np.empty(BODY_MAX, np.uint8)
    named = np.zeros(nslots, np.int64)          # it + 1 for the slots iteration `it` names
    touched = np.empty(nrec, np.int64)
    dbuf = np.empty(nslots * (DELTA_HEAD + IMG), np.uint8)
    dend = np.empty(nslots + 1, np.int64)
    Rr, U = np.empty(nl, np.int64), np.empty(nl, np.int64)
    dirty = np.zeros(NDW, np.bool_)
    pos = r = u0 = 0
    for it in range(iters):
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
        # the pool frame's iteration
        c = _pool_iter(it, sec, ctl, codes, S, B, T, kinds, k0, r, P, H, pwin, page, Pn, Hn, winn,
                       agen, lastnew, shots, meta, scr, pcur, payload, pbuf, p_at, p_ln, err)
        if c:
            return c
        P, Pn = Pn, P
        H, Hn = Hn, H
        pwin, winn = winn, pwin
        page, agen = agen, page
        flip = not flip
        # the unit frame: every candidate -- live, not slot 0, not named -- but a resting one
        # unless listed, in slot order, the listed ones in theirs
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
        c = _cells_iter(fst[0], fcols[0], sec, it, gt, copy, live, err)
        if c == 0:
            c = _orders_iter(fst[1], fcols[1], sec, it, gt, copy, live, err)
        if c == 0:
            c = _events_iter(fst[2], fcols[2], sec, it, gt, copy, live, err)
        if c == 0:
            c = _state_iter(fst[3], fcols[3], sec, it, gt, copy, live, err)
        if c:
            return c
        d = nsh = 0
        events = False
        for q in range(k0, r):
            kind, sc = kinds[q], secs[q]
            if kind == RK_UNIT_DELTA:
                pos = _put(out, pos, kind, dbuf, dend[d], dend[d + 1] - dend[d], err)
                d += 1
            elif sc == RCS_PROJ:
                pos = _put(out, pos, kind, pbuf, p_at[q], p_ln[q], err)
            elif sc == RCS_OTHER:
                at, ln = rec_at[q], rec_ln[q]
                if kind == RK_UNIT or kind == RK_UNIT_GONE:
                    s = np.int64(sec[at]) | np.int64(sec[at + 1]) << 8
                    live[s] = kind == RK_UNIT
                    if kind == RK_UNIT:
                        copy[s, :] = sec[at + 4:at + 4 + IMG]
                pos = _put(out, pos, kind, sec, at, ln, err)
            else:
                if sc == RCS_CELLS:
                    ln = _cells_rec(fst[0], fcols[0], sec, it, gt, events, copy, live, kind, body,
                                    err)
                elif sc == RCS_ORDERS:
                    ln = _orders_rec(fst[1], fcols[1], sec, it, gt, events, copy, live, kind, body,
                                     err)
                elif sc == RCS_EVENTS:
                    ln = _events_rec(fst[2], fcols[2], sec, it, gt, events, copy, live, kind, body,
                                     err)
                else:
                    ln = _state_rec(fst[3], fcols[3], sec, it, gt, events, copy, live, kind, body,
                                    err)
                if ln < 0:
                    return -ln
                if ln > BODY_MAX:
                    return _fail(err, E_F_BIG, sc, ln)
                pos = _put(out, pos, kind, body, 0, ln, err)
                # the next iteration's pool takes these shots (_pool_iter)
                if kind == RK_EV_SHOT and ln == SHOT and nsh < SHOTS_MAX:
                    shots[nsh, :] = body[:SHOT]
                    nsh += 1
            if pos < 0:
                return err[0]
            if sc == RCS_EVENTS:
                events = True
        meta[2] = nsh
        c = _cells_iter_end(fst[0], fcols[0], sec, it, gt, copy, live, err)
        if c == 0:
            c = _orders_iter_end(fst[1], fcols[1], sec, it, gt, copy, live, err)
        if c == 0:
            c = _events_iter_end(fst[2], fcols[2], sec, it, gt, copy, live, err)
        if c == 0:
            c = _state_iter_end(fst[3], fcols[3], sec, it, gt, copy, live, err)
        if c:
            return c
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
                lstep[s, :] = 0
            else:
                age[s] = 0
            rest[s] = False
    if flip:
        # the caller's arrays are the scratch ones now: the pool's state goes back into them
        Pn[:] = P
        Hn[:] = H
        winn[:] = pwin
        agen[:] = page
    c = _cells_close(fst[0], fcols[0], sec, err)
    if c == 0:
        c = _orders_close(fst[1], fcols[1], sec, err)
    if c == 0:
        c = _events_close(fst[2], fcols[2], sec, err)
    if c == 0:
        c = _state_close(fst[3], fcols[3], sec, err)
    if c:
        return c
    if pos != len(out):
        return _fail(err, E_PAYLOAD, pos, len(out))
    return 0


# ---- the pool frame ---------------------------------------------------------------------------
#
# The projectile pool coded by identity (tagpu_reccodec.c, THE POOL FRAME; tagpu_recfmt.h, RCS_PROJ):
# per iteration the dead records, the survivors predicted from their own last images, the births
# and the changed stale slots each against a reference row. Rows are int64 arrays of 27 dwords
# held in [0, 2^32): `& M32` is the dword's arithmetic.

# the survivor step's dword order: the heading, pitch and speed before the velocity, the velocity
# before the position
_PORDER = np.array([13, 14, 15, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 10, 11, 12] + list(range(16, PDW)),
                   np.int64)
P_OUT, P_OLD, P_ADV, P_EV = range(4)        # a reference's sections


@_jit
def _gvel(x, g):
    """The guided weapon's velocity (vx, vy, vz) from row x's heading (+0x36), pitch (+0x38) and
    speed (+0x3A), into g: the projectile tick's rule 0x49BA74..0x49BABE."""
    heading = x[13] >> 16
    pitch = x[14] & 0xFFFF
    speed = s32((x[14] >> 16) | ((x[15] & 0xFFFF) << 16))
    h = table_mul(pitch, speed, 0x4020)
    g[0] = (-table_mul(heading, h, 0x20)) & M32
    g[1] = table_mul(pitch, speed, 0x20) & M32
    g[2] = (-table_mul(heading, h, 0x4020)) & M32


@_jit
def _held(x, g):
    """Row x's own velocity follows the guided rule (g is scratch)."""
    _gvel(x, g)
    return g[0] == x[7] and g[1] == x[8] and g[2] == x[9]


@_jit
def _dist(a, b):
    return min((a - b) & M32, (b - a) & M32)


@_jit
def _survive(L, L2, a, m, win, res, U, g):
    """Old record m's image this iteration into U: its last image L, the one before L2 (age a),
    its winners `win` (updated) and the residuals by dword."""
    held = _held(L, g)
    for i in range(PDW):
        d = _PORDER[i]
        l1 = L[d]
        l2 = L2[d] if a >= 2 else l1
        lin = (2 * l1 - l2) & M32
        if 1 <= d <= 3:
            r0 = (l1 + U[d + 6] + ((l1 - l2 - L[d + 6]) if a >= 2 else 0)) & M32
        elif 7 <= d <= 9:
            if d == 7 and held:
                _gvel(U, g)
            r0 = g[d - 7] if held else l1
        elif d == 25:
            r0 = (lin & 0xFFFFFF) | ((m & 0xFF) << 24)
        elif d == 26:
            r0 = (l1 & 0xFFFFFF00) | ((m >> 8) & 0xFF)
        else:
            r0 = l1
        w = win[d]
        u = ((r0 if w == 0 else l1 if w == 1 else lin) + res[d]) & M32
        U[d] = u
        e0, e1, e2 = _dist(u, r0), _dist(u, l1), _dist(u, lin)
        win[d] = 0 if e0 <= e1 and e0 <= e2 else 1 if e1 <= e2 else 2


@_jit
def _ev_row(b, lastnew, row):
    """A shot event's row: the last birth of its weapon, with the shot's start (the build's,
    body +0x01) as the position and the tail, its target point, its weapon ID and its two unit
    IDs."""
    w = np.int64(b[0x19]) | (np.int64(b[0x23]) >> 4) << 8
    by = np.empty(PIMG, np.uint8)
    for d in range(PDW):
        v = lastnew[w + 1, d]
        for k in range(4):
            by[4 * d + k] = (v >> (8 * k)) & 0xFF
    for k in range(4):
        by[k] = ((w + 1) >> (8 * k)) & 0xFF
    by[0x04:0x10] = b[0x01:0x0D]
    by[0x10:0x1C] = b[0x01:0x0D]
    by[0x28:0x34] = b[0x0D:0x19]
    by[0x4E], by[0x4F], by[0x50], by[0x51] = b[0x1F], b[0x20], 0, 0
    by[0x52], by[0x53], by[0x54], by[0x55] = b[0x21], b[0x22], 0, 0
    for d in range(PDW):
        row[d] = (np.int64(by[4 * d]) | np.int64(by[4 * d + 1]) << 8 | np.int64(by[4 * d + 2]) << 16 |
                  np.int64(by[4 * d + 3]) << 24)


@_jit
def _ref_row(R, stamp, s, res, out, g):
    """The reference-coded row of R (stamped with s for a birth) and its residuals into out: R
    plus the residual for every dword, the velocity's base the guided rule of out itself when R
    follows the rule."""
    base = R.copy()
    if stamp:
        base[25] = (base[25] & 0x00FFFFFF) | ((s & 0xFF) << 24)
        base[26] = (base[26] & 0xFFFFFF00) | ((s >> 8) & 0xFF)
    for d in range(PDW):
        if d < 7 or d > 9:
            out[d] = (base[d] + res[d]) & M32
    if _held(base, g):
        _gvel(out, g)
        for i in range(3):
            base[7 + i] = g[i]
    for i in range(3):
        out[7 + i] = (base[7 + i] + res[7 + i]) & M32


@_jit
def _pool_ctl(sec, at, end, iters, n, c0, h0, ctl, err):
    """The pool frame's control values, bounded against the pool's n records: ctl[it] is
    (offset, c1, h1, nd, nt). Returns (offset after them, survivors, births, tail slots), the
    offset -code when refused."""
    v, at = _varint(sec, at, end)
    if at < 0 or v != iters:
        return -_fail(err, E_P_ITERS, iters), 0, 0, 0
    ns = nb = nt = 0
    for it in range(iters):
        ctl[it, 0] = at
        v, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_P_CUT, it), 0, 0, 0
        c1 = c0 + ((v >> 1) ^ -(v & 1))
        if c1 < 0 or c1 > n:
            return -_fail(err, E_P_COUNT, it, c1, n), 0, 0, 0
        v, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_P_CUT, it), 0, 0, 0
        h1 = h0 + v
        if h1 > n:
            return -_fail(err, E_P_EXTENT, it, h1, n), 0, 0, 0
        if c1 > h1:
            return -_fail(err, E_P_OVER, it, c1, h1), 0, 0, 0
        nd, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_P_CUT, it), 0, 0, 0
        if nd > c0:
            return -_fail(err, E_P_DEADS, it, nd, c0), 0, 0, 0
        slot = -1
        for k in range(nd):
            v, at = _varint(sec, at, end)
            if at < 0:
                return -_fail(err, E_P_CUT, it), 0, 0, 0
            slot += 1 + v
            if slot >= c0:
                return -_fail(err, E_P_DEAD, it, slot, c0), 0, 0, 0
        if c0 - nd > c1:
            return -_fail(err, E_P_SURV, it, c0 - nd, c1), 0, 0, 0
        k, at = _varint(sec, at, end)
        if at < 0:
            return -_fail(err, E_P_CUT, it), 0, 0, 0
        if k > h1 - c1:
            return -_fail(err, E_P_TAILS, it, k), 0, 0, 0
        slot = c1 - 1
        for i in range(k):
            v, at = _varint(sec, at, end)
            if at < 0:
                return -_fail(err, E_P_CUT, it), 0, 0, 0
            slot += 1 + v
            if slot >= h1:
                return -_fail(err, E_P_TAIL, it, slot, h1), 0, 0, 0
        ctl[it, 1], ctl[it, 2], ctl[it, 3], ctl[it, 4] = c1, h1, nd, k
        ns += c0 - nd
        nb += c1 - (c0 - nd)
        nt += k
        c0, h0 = c1, h1
    return at, ns, nb, nt


@_jit
def _pool_ref(code, anchor, length, it, err):
    """A reference code's (section, row), or (-1, 0) for the zero row; (-code, 0) past its section."""
    if code == 0:
        return -1, 0
    sc = (code - 1) & 3
    z = (code - 1) >> 2
    kk = anchor[sc] + ((z >> 1) ^ -(z & 1))
    if kk < 0 or kk >= length[sc]:
        return -_fail(err, E_P_REF, it, kk, sc), 0
    return sc, kk


@_jit
def _pool_body(kind, j, old, new, out_buf, pos):
    """Slot j's RK_PROJ or RK_PROJ_DELTA body at out_buf[pos:]: its length."""
    out_buf[pos], out_buf[pos + 1] = j & 0xFF, (j >> 8) & 0xFF
    if kind == RK_PROJ:
        out_buf[pos + 2] = out_buf[pos + 3] = 0
        at = pos + 4
        for d in range(PDW):
            for k in range(4):
                out_buf[at + k] = (new[d] >> (8 * k)) & 0xFF
            at += 4
        return 4 + PIMG
    bits, at = 0, pos + 8
    for d in range(PDW):
        if new[d] != old[d]:
            bits |= 1 << d
            for k in range(4):
                out_buf[at + k] = (new[d] >> (8 * k)) & 0xFF
            at += 4
    for k in range(4):
        out_buf[pos + 2 + k] = (bits >> (8 * k)) & 0xFF
    out_buf[pos + 6] = out_buf[pos + 7] = 0
    return at - pos


@_jit
def _pool_iter(it, sec, ctl, codes, S, B, T, kinds, r0, r1, P, H, win, age, Pn, Hn, winn, agen,
               lastnew, shots, meta, scr, cur, payload, out_buf, out_at, out_ln, err):
    """Iteration `it` of the pool frame, its records kinds[r0:r1]: the pool after it into Pn,
    Hn, winn and agen from the pool before it (P, H, win, age; meta: its count and extent, and
    the last iteration's shots, which `shots` holds), the body of each of its projectile records
    into out_buf at out_at[r], out_ln[r] (r the record's index), meta's count and extent moved
    on. scr is the block's scratch, cur its cursors: the survivors', births' and tail slots' rows
    so far, the codes, out_buf's end and the records' bytes. 0, or the refusal's code."""
    adv, advd, ev, alive, newix, m, tail, chg, zero, R, g, wsc, anchor, length = scr
    sp, bp, tp, cp, pos, total = cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]
    end = len(sec)
    c0, h0, ne = meta[0], meta[1], meta[2]
    c1, h1, nd, nt = ctl[it, 1], ctl[it, 2], ctl[it, 3], ctl[it, 4]
    at = ctl[it, 0]
    for k in range(3):
        v, at = _varint(sec, at, end)
    alive[:c0] = True
    slot = -1
    for k in range(nd):
        v, at = _varint(sec, at, end)
        slot += 1 + v
        alive[slot] = False
    v, at = _varint(sec, at, end)
    slot = c1 - 1
    for k in range(nt):
        v, at = _varint(sec, at, end)
        slot += 1 + v
        tail[k] = slot
    ns = 0
    for k in range(c0):
        newix[k] = -1
        if alive[k]:
            m[ns] = k
            newix[k] = ns
            ns += 1
    for k in range(ne):
        _ev_row(shots[k], lastnew, ev[k])
    # this iteration's heads and slot records
    heads = slots = 0
    for q in range(r0, r1):
        kind = kinds[q]
        if kind == RK_PROJ_HEAD:
            heads += 1
        elif kind == RK_PROJ or kind == RK_PROJ_DELTA:
            slots += 1
    if heads > 1:
        return _fail(err, E_P_HEADS, it, heads)
    if heads == 0 and (c1 != c0 or h1 != h0):
        return _fail(err, E_P_NOHEAD, it)
    Pn[:h0] = P[:h0]
    for j in range(ns):
        mo = m[j]
        winn[j] = win[mo]
        _survive(P[mo], H[mo], age[mo], mo, winn[j], S[sp], Pn[j], g)
        sp += 1
        Hn[j] = P[mo]
        agen[j] = min(age[mo] + 1, 2)
    advd[:c0] = False
    evnext = 0
    for j in range(ns, c1):
        anchor[P_OUT], anchor[P_OLD], anchor[P_ADV] = j - 1, c0 - 1, c0 - 1
        anchor[P_EV] = evnext if evnext < ne else ne - 1
        length[P_OUT], length[P_OLD], length[P_ADV], length[P_EV] = j, h0, c0, ne
        sc, kk = _pool_ref(codes[cp], anchor, length, it, err)
        cp += 1
        if sc < -1:
            return err[0]
        if sc == -1:
            R[:] = zero
        elif sc == P_OUT:
            R[:] = Pn[kk]
        elif sc == P_OLD:
            R[:] = P[kk]
        elif sc == P_ADV:
            if not advd[kk]:
                wsc[:] = win[kk]
                _survive(P[kk], H[kk], age[kk], kk, wsc, zero, adv[kk], g)
                advd[kk] = True
            R[:] = adv[kk]
        else:
            R[:] = ev[kk]
            evnext = kk + 1
        _ref_row(R, True, (c0 + j - ns) & 0xFFFF, B[bp], Pn[j], g)
        bp += 1
        if 1 <= Pn[j, 0] <= WEAPON_IDS:
            lastnew[Pn[j, 0]] = Pn[j]
        winn[j] = 0
        agen[j] = 1
    for i in range(nt):
        tj = tail[i]
        if tj < c0:
            anchor[P_OUT] = newix[tj] if newix[tj] >= 0 else min(tj, c1 - 1)
        else:
            anchor[P_OUT] = min(ns + (tj - c0), c1 - 1)
        anchor[P_OLD], anchor[P_ADV], anchor[P_EV] = min(tj, h0 - 1), min(tj, c0 - 1), ne - 1
        length[P_OUT], length[P_OLD], length[P_ADV], length[P_EV] = c1, h0, c0, ne
        sc, kk = _pool_ref(codes[cp], anchor, length, it, err)
        cp += 1
        if sc < -1:
            return err[0]
        if sc == -1:
            R[:] = zero
        elif sc == P_OUT:
            R[:] = Pn[kk]
        elif sc == P_OLD:
            R[:] = P[kk]
        elif sc == P_ADV:
            if not advd[kk]:
                wsc[:] = win[kk]
                _survive(P[kk], H[kk], age[kk], kk, wsc, zero, adv[kk], g)
                advd[kk] = True
            R[:] = adv[kk]
        else:
            R[:] = ev[kk]
        _ref_row(R, False, 0, T[tp], Pn[tj], g)
        tp += 1
    # the changed slots, ascending: below the count, then the tail (the pool is zero from h0 on)
    nc = 0
    for j in range(c1):
        if (Pn[j] != P[j]).any():
            chg[nc] = j
            nc += 1
    for i in range(nt):
        if (Pn[tail[i]] != P[tail[i]]).any():
            chg[nc] = tail[i]
            nc += 1
    if slots != nc:
        return _fail(err, E_P_SLOTS, it, nc, slots)
    q2 = 0
    for q in range(r0, r1):
        kind = kinds[q]
        if kind == RK_PROJ_HEAD:
            for k in range(4):
                out_buf[pos + k] = (c1 >> (8 * k)) & 0xFF
                out_buf[pos + 4 + k] = (h1 >> (8 * k)) & 0xFF
            ln = 8
        elif kind == RK_PROJ or kind == RK_PROJ_DELTA:
            j = chg[q2]
            q2 += 1
            ln = _pool_body(kind, j, P[j], Pn[j], out_buf, pos)
        else:
            continue
        total += (4 + ln + 3) & ~3
        if total > payload:
            return _fail(err, E_PAST_PAYLOAD)
        out_at[q], out_ln[q] = pos, ln
        pos += ln
    agen[c1:] = 0
    meta[0], meta[1] = c1, h1
    cur[0], cur[1], cur[2], cur[3], cur[4], cur[5] = sp, bp, tp, cp, pos, total
    return 0


# ---- the record frames' columns (tagpu_recframe.h, THE COLUMNS) ----------------------------------
#
# A record frame's section is a u8 scheme, a varint column count, each column's varint length,
# then the columns, which fill the section; a reader takes only its own scheme and count and reads
# every column to its end. A frame's columns are the rows of its `cols` (the next byte, the end),
# which _frame_open sets and its kernels read on.
#
# A record frame's region holds its state -- a tuple of numpy arrays, which _X_new makes for a
# usable segment's params, _X_restart clears at every reset and _X_clone copies -- and the kernels
# _iterations calls: _X_open (the section), per iteration _X_iter, _X_rec for each record of its
# kinds (its body into `body`, returning its length or -code) and _X_iter_end, and _X_close after
# the last. Each returns 0 or the refusal's code.

FRAME_COLS_MAX = 32                        # RECF_COLS_MAX
BODY_MAX = 0xFFFF                          # RECF_BODY_MAX: a body RecHdr's u16 length holds


@_jit
def _frame_open(sec, at, end, scheme, ncols, cols, err, s):
    """Section s, sec[at:end], as `ncols` columns of `scheme` into cols: 0, or the code."""
    if ncols < 1 or ncols > FRAME_COLS_MAX:
        return _fail(err, E_F_NCOLS, s, ncols)
    if at >= end:
        return _fail(err, E_F_SCHEME, s, -1, scheme)
    if sec[at] != scheme:
        return _fail(err, E_F_SCHEME, s, sec[at], scheme)
    v, at = _varint(sec, at + 1, end)
    if at < 0 or v != ncols:
        return _fail(err, E_F_NCOLS, s, ncols)
    for k in range(ncols):
        v, at = _varint(sec, at, end)
        if at < 0:
            return _fail(err, E_F_TABLE, s)
        cols[k, 1] = v
    for k in range(ncols):
        ln = cols[k, 1]
        if ln > end - at:
            return _fail(err, E_F_PAST, s, k)
        cols[k, 0] = at
        at += ln
        cols[k, 1] = at
    if at != end:
        return _fail(err, E_F_FILL, s, end - at)
    return 0


@_jit
def _frame_close(cols, ncols, err, s):
    """Every column of section s read to its end: 0, or the code."""
    for k in range(ncols):
        if cols[k, 0] != cols[k, 1]:
            return _fail(err, E_F_LEFT, s, k, cols[k, 1] - cols[k, 0])
    return 0


@_jit
def _col_varint(sec, cols, k):
    """The next varint of column k: (value, True), or (0, False) when it runs past the column."""
    v, at = _varint(sec, cols[k, 0], cols[k, 1])
    if at < 0:
        return np.int64(0), False
    cols[k, 0] = at
    return v, True


@_jit
def _col_u8(sec, cols, k):
    """The next byte of column k: (value, True), or (0, False) at the column's end."""
    if cols[k, 0] >= cols[k, 1]:
        return np.int64(0), False
    cols[k, 0] += 1
    return np.int64(sec[cols[k, 0] - 1]), True


@_jit
def _col_bytes(sec, cols, k, dst, at, n):
    """The next n bytes of column k into dst[at:]: False when they run past the column or past
    dst, so a frame's body is bounded before the write (recf_body_put)."""
    if n < 0 or n > cols[k, 1] - cols[k, 0] or at < 0 or n > dst.shape[0] - at:
        return False
    dst[at:at + n] = sec[cols[k, 0]:cols[k, 0] + n]
    cols[k, 0] += n
    return True


@_jit
def _zz32(r):
    """A 32-bit residual's zigzag (tagpu_recframe.h zz32), r in [0, 2^32)."""
    return 2 * r if r < 0x80000000 else 2 * (0x100000000 - r) - 1


@_jit
def _unzz32(z):
    return ((z >> 1) ^ -(z & 1)) & M32


# ---- THE CELLS FRAME (tagpu_recframe_cells.c) ----------------------------------------------------
#
# Scheme 2, nine columns -- flag, rec, sym, f0..f5 -- coding each RK_CELLS against G, the grid's
# copy, and an iteration's images against T, the last iteration's tick input (the C says the bytes,
# the engine facts and the bounds). The state: (W and H, G, T's and T''s records' first cells, cell
# counts and cells, the scalars _CM_*). Each of T and T' is a row of the tables: row `cur` is T.
# A restart empties T and T' and the scalars; G is this reading's own grid, every image it has
# read (a sectioned block's through _cells_rec, a records block's through _cells_image), which is
# the world every walk restarts the frame from (tagpu_reccodec.c, RESET POINTS).

(E_CELLS_FLAG, E_CELLS_TPAST, E_CELLS_TLEFT, E_CELLS_RUNS, E_CELLS_AT, E_CELLS_N, E_CELLS_ROW,
 E_CELLS_SYMS, E_CELLS_SYM, E_CELLS_FIELD, E_CELLS_BIG) = range(100, 111)
_WHY.update({
    E_CELLS_FLAG: "iteration {0}: the cells frame's flag cut short or past 1",
    E_CELLS_TPAST: "iteration {0}: an image past the last tick input's {1} records",
    E_CELLS_TLEFT: "iteration {0}: its images are {1} of the last tick input's {2} records",
    E_CELLS_RUNS: "the cells frame's runs cut short",
    E_CELLS_AT: "an RK_CELLS at cell {0} of a {1}-cell grid",
    E_CELLS_N: "an RK_CELLS of {0} cells",
    E_CELLS_ROW: "an RK_CELLS of {0} cells from column {1} of {2}",
    E_CELLS_SYMS: "the cells frame's syms cut short",
    E_CELLS_SYM: "a cell's sym {0:#x}",
    E_CELLS_FIELD: "the cells frame's field {0} cut short",
    E_CELLS_BIG: "an RK_CELLS of {0} cells past 0xFFFF bytes"})
_CELLS_SCHEME, _CELLS_COLS = 2, 9
_CC_FLAG, _CC_REC, _CC_SYM, _CC_F0 = 0, 1, 2, 3
assert 1 <= _CELLS_COLS <= FRAME_COLS_MAX
_CB = tarec.REC_CELL_BYTES
_CELLS_MAX = tarec.REC_CELLS_MAX
_CELLS_HELD = 4 * tarec.REC_CELLS_MAX      # CELLS_HELD: T's cells at most, past it T is empty
_CELLS_A_EMPTY = 0x40
_CELL_FIELD = np.array([0, 2, 4, 8, 10, 12, _CB], np.int64)
# the scalars: T's row; per row its records, cells and over-the-bound flag; the iteration's
# image seen, flag, T's next record and its first cell, and each list's last end
_CM_CUR, _CM_NREC, _CM_NCELL, _CM_OVER, _CM_IMAGED, _CM_FLAG, _CM_TNEXT, _CM_TCELL, _CM_E = (
    0, 1, 3, 5, 7, 8, 9, 10, 11)


def _cells_new(prm):
    """The cells frame's state for a usable segment's params."""
    wh = prm.gw * prm.gh
    return (np.array([prm.gw, prm.gh], np.int64), np.zeros(wh * _CB, np.uint8),
            np.zeros((2, _CELLS_HELD), np.int64),
            np.zeros((2, _CELLS_HELD), np.int64), np.zeros((2, _CELLS_HELD * _CB), np.uint8),
            np.zeros(13, np.int64))


def _cells_restart(st):
    """T and T' empty; G stays, this reading's grid: the world a reset restarts from."""
    st[5][:] = 0


def _cells_image(st, payload, b, ln):
    """A records block's image, the body at payload[b:b + ln], onto G under the reader's bounds
    (tagpu_recreader.c, cells_ok); one it refuses is skipped."""
    if ln < 8:
        return
    x, z, n, pad = struct.unpack_from("<4H", payload, b)
    W, H = int(st[0][0]), int(st[0][1])
    if not 1 <= n <= _CELLS_MAX or pad or ln != 8 + n * _CB or x + n > W or z >= H:
        return
    p = (z * W + x) * _CB
    st[1][p:p + n * _CB] = np.frombuffer(payload, np.uint8, n * _CB, b + 8)


def _cells_clone(st):
    return tuple(a.copy() for a in st)


@_jit
def _cells_open(st, cols, sec, at, end, iters, err):
    return _frame_open(sec, at, end, _CELLS_SCHEME, _CELLS_COLS, cols, err, RCS_CELLS)


@_jit
def _cells_iter(st, cols, sec, it, gt, copy, live, err):
    m = st[5]
    m[_CM_IMAGED] = m[_CM_FLAG] = m[_CM_TNEXT] = m[_CM_TCELL] = 0
    m[_CM_E] = m[_CM_E + 1] = 0
    t = m[_CM_CUR] ^ 1
    m[_CM_NREC + t] = m[_CM_NCELL + t] = m[_CM_OVER + t] = 0
    return 0


@_jit
def _cells_head(body, x, z, n):
    body[0], body[1], body[2], body[3] = x & 0xFF, x >> 8, z & 0xFF, z >> 8
    body[4], body[5], body[6], body[7] = n & 0xFF, n >> 8, 0, 0


@_jit
def _cells_code(st, cols, sec, lst, body, err):
    """One coded record of list `lst` (0 the images, 1 the tick input) into body: (its first cell,
    its cells), or (-code, 0)."""
    dims, G, m = st[0], st[1], st[5]
    W = dims[0]
    wh = dims[0] * dims[1]
    r, ok = _col_varint(sec, cols, _CC_REC)
    if not ok:
        return -_fail(err, E_CELLS_RUNS), 0
    m1, ok = _col_varint(sec, cols, _CC_REC)
    if not ok:
        return -_fail(err, E_CELLS_RUNS), 0
    p = (m[_CM_E + lst] + _unzz32(r)) & M32
    if p >= wh:
        return -_fail(err, E_CELLS_AT, p, wh), 0
    if m1 >= _CELLS_MAX:
        return -_fail(err, E_CELLS_N, m1 + 1), 0
    n = m1 + 1
    x = p % W
    if x + n > W:
        return -_fail(err, E_CELLS_ROW, n, x, W), 0
    if 8 + n * _CB > BODY_MAX:
        return -_fail(err, E_CELLS_BIG, n), 0
    m[_CM_E + lst] = p + n
    _cells_head(body, x, p // W, n)
    for k in range(n):
        q = p + k
        o = 8 + k * _CB
        body[o:o + _CB] = G[q * _CB:(q + 1) * _CB]
        s, ok = _col_u8(sec, cols, _CC_SYM)
        if not ok:
            return -_fail(err, E_CELLS_SYMS), 0
        if s == _CELLS_A_EMPTY:
            body[o] = body[o + 1] = 0
            continue
        if s > 0x3F:
            return -_fail(err, E_CELLS_SYM, s), 0
        for f in range(6):
            if s >> f & 1:
                lo = _CELL_FIELD[f]
                if not _col_bytes(sec, cols, _CC_F0 + f, body, o + lo, _CELL_FIELD[f + 1] - lo):
                    return -_fail(err, E_CELLS_FIELD, f), 0
    return p, n


@_jit
def _cells_rec(st, cols, sec, it, gt, events, copy, live, kind, body, err):
    dims, G, tp, tn, tc, m = st
    W = dims[0]
    cur = m[_CM_CUR]
    if not events:
        if not m[_CM_IMAGED]:
            m[_CM_IMAGED] = 1
            v, ok = _col_u8(sec, cols, _CC_FLAG)
            if not ok or v > 1:
                return -_fail(err, E_CELLS_FLAG, it)
            m[_CM_FLAG] = v
        if m[_CM_FLAG]:
            j = m[_CM_TNEXT]
            if j >= m[_CM_NREC + cur]:
                return -_fail(err, E_CELLS_TPAST, it, m[_CM_NREC + cur])
            p, n = tp[cur, j], tn[cur, j]
            if 8 + n * _CB > BODY_MAX:
                return -_fail(err, E_CELLS_BIG, n)
            _cells_head(body, p % W, p // W, n)
            a = m[_CM_TCELL] * _CB
            body[8:8 + n * _CB] = tc[cur, a:a + n * _CB]
            m[_CM_TNEXT] += 1
            m[_CM_TCELL] += n
        else:
            p, n = _cells_code(st, cols, sec, 0, body, err)
            if p < 0:
                return p
        G[p * _CB:(p + n) * _CB] = body[8:8 + n * _CB]
    else:
        p, n = _cells_code(st, cols, sec, 1, body, err)
        if p < 0:
            return p
        # into T', unless it passes CELLS_HELD
        t = cur ^ 1
        c = m[_CM_NCELL + t]
        if not m[_CM_OVER + t]:
            if n > _CELLS_HELD - c:
                m[_CM_OVER + t] = 1
            else:
                r = m[_CM_NREC + t]
                tp[t, r], tn[t, r] = p, n
                tc[t, c * _CB:(c + n) * _CB] = body[8:8 + n * _CB]
                m[_CM_NREC + t] = r + 1
                m[_CM_NCELL + t] = c + n
    return 8 + n * _CB


@_jit
def _cells_iter_end(st, cols, sec, it, gt, copy, live, err):
    m = st[5]
    cur = m[_CM_CUR]
    if m[_CM_FLAG] and m[_CM_TNEXT] != m[_CM_NREC + cur]:
        return _fail(err, E_CELLS_TLEFT, it, m[_CM_TNEXT], m[_CM_NREC + cur])
    # T takes T', empty when it passed CELLS_HELD
    t = cur ^ 1
    if m[_CM_OVER + t]:
        m[_CM_NREC + t] = m[_CM_NCELL + t] = 0
    m[_CM_CUR] = t
    m[_CM_NREC + cur] = m[_CM_NCELL + cur] = m[_CM_OVER + cur] = 0
    return 0


@_jit
def _cells_close(st, cols, sec, err):
    return _frame_close(cols, _CELLS_COLS, err, RCS_CELLS)


# ---- THE ORDERS FRAME (tagpu_recframe_orders.c) --------------------------------------------------
#
# Scheme 1, 26 columns: slot, key, mask, d0..d21, whole (the C file's THE BYTES). The state, cleared
# at every reset: per (slot, list) its place in the arena and its kept list -- `kept` rows of (off,
# cap, nodes + 1, 0 for none) -- the arena of node images, and (held, the iteration's previous
# slot). The arena is ORDERS_HELD nodes from the start, so nothing here can fail an allocation; the
# places and what is kept follow the C's rule from the records alone.

(E_ORDERS_CUT, E_ORDERS_SLOT, E_ORDERS_KEY, E_ORDERS_NODES, E_ORDERS_KEPT,
 E_ORDERS_MASK) = range(150, 156)
_WHY.update({E_ORDERS_CUT: "the orders frame's column {0} cut short",
             E_ORDERS_SLOT: "an RK_ORDERS for slot {0}",
             E_ORDERS_KEY: "an RK_ORDERS key {0}",
             E_ORDERS_NODES: "an RK_ORDERS of nodes [{0}, +{1}) of a list of {2}",
             E_ORDERS_KEPT: "slot {0} list {1}'s RK_ORDERS coded against no kept list",
             E_ORDERS_MASK: "an order node mask {0} past 22 bits"})
_ORDERS_SCHEME = 1
_ORDERS_HELD = 65536                       # RECC_ORDERS_HELD
_O_NODE = tarec.REC_ORDER_IMAGE            # 0x58
_O_DW = _O_NODE // 4                       # 22
_O_MAX, _O_CHUNK = 4096, 512               # REC_ORDERS_MAX, REC_ORDERS_CHUNK
_O_SLOT, _O_KEY, _O_MASK, _O_D0 = 0, 1, 2, 3
_O_WHOLE = _O_D0 + _O_DW
_ORDERS_COLS = _O_WHOLE + 1
assert 1 <= _ORDERS_COLS <= FRAME_COLS_MAX
_K_LIST, _K_WHOLE, _K_CHUNK = 1, 2, 4


def _orders_new(prm):
    """The orders frame's state for a usable segment's params: (kept, arena, (held, prev))."""
    return (np.zeros((prm.nslots * 2, 3), np.int64), np.zeros(_ORDERS_HELD * _O_NODE, np.uint8),
            np.zeros(2, np.int64))


def _orders_restart(st):
    st[0][...] = 0
    st[2][...] = 0


def _orders_clone(st):
    kept, arena, meta = st
    a = np.zeros_like(arena)
    n = int(meta[0]) * _O_NODE
    a[:n] = arena[:n]
    return kept.copy(), a, meta.copy()


@_jit
def _orders_open(st, cols, sec, at, end, iters, err):
    return _frame_open(sec, at, end, _ORDERS_SCHEME, _ORDERS_COLS, cols, err, RCS_ORDERS)


@_jit
def _orders_iter(st, cols, sec, it, gt, copy, live, err):
    st[2][1] = 0
    return 0


@_jit
def _orders_rec(st, cols, sec, it, gt, events, copy, live, kind, body, err):
    kept, arena, meta = st
    z, ok = _col_varint(sec, cols, _O_SLOT)
    if not ok:
        return -_fail(err, E_ORDERS_CUT, _O_SLOT)
    slot = (meta[1] + _unzz32(z)) & M32
    if not 1 <= slot < len(live):
        return -_fail(err, E_ORDERS_SLOT, slot)
    meta[1] = slot
    key, ok = _col_u8(sec, cols, _O_KEY)
    if not ok:
        return -_fail(err, E_ORDERS_CUT, _O_KEY)
    if key & ~(_K_LIST | _K_WHOLE | _K_CHUNK) or (key & _K_CHUNK and not key & _K_WHOLE):
        return -_fail(err, E_ORDERS_KEY, key)
    e = slot * 2 + (key & _K_LIST)
    first = np.int64(0)
    if key & _K_WHOLE:
        total, ok = _col_varint(sec, cols, _O_KEY)
        if not ok:
            return -_fail(err, E_ORDERS_CUT, _O_KEY)
        k = total
        if key & _K_CHUNK:
            first, ok = _col_varint(sec, cols, _O_KEY)
            if ok:
                k, ok = _col_varint(sec, cols, _O_KEY)
            if not ok:
                return -_fail(err, E_ORDERS_CUT, _O_KEY)
        if total > _O_MAX or k > _O_CHUNK or first + k > total or (total and not k):
            return -_fail(err, E_ORDERS_NODES, first, k, total)
    else:
        if kept[e, 2] == 0:
            return -_fail(err, E_ORDERS_KEPT, slot, key & _K_LIST)
        total = k = kept[e, 2] - 1
    # every write into body is bounded first: _col_bytes by itself, the rest here
    if 8 + k * _O_NODE > BODY_MAX:
        return -_fail(err, E_F_BIG, RCS_ORDERS, 8 + k * _O_NODE)
    body[0], body[1], body[2], body[3] = slot & 0xFF, slot >> 8, key & _K_LIST, 0
    body[4], body[5], body[6], body[7] = total & 0xFF, total >> 8, first & 0xFF, first >> 8
    if key & _K_WHOLE:
        if not _col_bytes(sec, cols, _O_WHOLE, body, 8, k * _O_NODE):
            return -_fail(err, E_ORDERS_CUT, _O_WHOLE)
    else:
        base = kept[e, 0] * _O_NODE
        for i in range(k):
            m, ok = _col_varint(sec, cols, _O_MASK)
            if not ok:
                return -_fail(err, E_ORDERS_CUT, _O_MASK)
            if m >> _O_DW:
                return -_fail(err, E_ORDERS_MASK, m)
            now = 8 + i * _O_NODE
            was = base + i * _O_NODE
            for d in range(_O_DW):
                if m >> d & 1:
                    if not _col_bytes(sec, cols, _O_D0 + d, body, now + 4 * d, 4):
                        return -_fail(err, E_ORDERS_CUT, _O_D0 + d)
                else:
                    body[now + 4 * d:now + 4 * d + 4] = arena[was + 4 * d:was + 4 * d + 4]
    # what is kept (tagpu_recframe_orders.c keep)
    if first or k != total:
        kept[e, 2] = 0
    else:
        fits = True
        if total > kept[e, 1]:
            if total > _ORDERS_HELD - meta[0]:
                fits = False
            else:
                kept[e, 0], kept[e, 1] = meta[0], total
                meta[0] += total
        if fits:
            a = kept[e, 0] * _O_NODE
            arena[a:a + total * _O_NODE] = body[8:8 + total * _O_NODE]
            kept[e, 2] = total + 1
        else:
            kept[e, 2] = 0
    return 8 + k * _O_NODE


@_jit
def _orders_iter_end(st, cols, sec, it, gt, copy, live, err):
    return 0


@_jit
def _orders_close(st, cols, sec, err):
    return _frame_close(cols, _ORDERS_COLS, err, RCS_ORDERS)


# ---- THE EVENTS FRAME (tagpu_recframe_events.c) --------------------------------------------------
#
# Scheme 1, _EV_COLS columns, each event's fields coded against the last unit ID, the unit copy and
# the events since the reset: THE BYTES, THE STATE and THE BOUNDS are the C file's. The state:
# misc (the last ID, the names, the texts and the open deaths held), the two dictionaries and
# their lengths, per slot its four (script index, arguments) entries and their count and next
# replaced, its last spray and whether it sprayed, per root its last site and whether it has one,
# and the open deaths.

E_EV_CUT, E_EV_SHOOTER, E_EV_TARGET, E_EV_ARGFLAG, E_EV_TEXT, E_EV_SPRAY, E_EV_NEST, E_EV_MASK, \
    E_EV_NODEATH, E_EV_ROOT, E_EV_SAME, E_EV_KIND, E_EV_BODY = range(200, 213)
_WHY[E_EV_CUT] = "an event (kind {0}) cut short in the events frame's column {1}"
_WHY[E_EV_SHOOTER] = "a shot's shooter ID {0} is past 0xFFFF"
_WHY[E_EV_TARGET] = "a shot's target ID {0} is past 0xFFFE"
_WHY[E_EV_ARGFLAG] = "an argument flag (kind {0}) is not 0 or 1"
_WHY[E_EV_TEXT] = "a name or text (kind {0}) is past its dictionary or its bound"
_WHY[E_EV_SPRAY] = "spray mode {0} for unit ID {1}"
_WHY[E_EV_NEST] = "deaths nested past {0}"
_WHY[E_EV_MASK] = "a death's mask names dword {0}"
_WHY[E_EV_NODEATH] = "an RK_EV_DEATH_END with no death open"
_WHY[E_EV_ROOT] = "feature root {0}"
_WHY[E_EV_SAME] = "a feature's same-site flag for root {0} with no site"
_WHY[E_EV_KIND] = "record kind {0} in the events frame"
_WHY[E_EV_BODY] = "a body of {0} bytes, under the longest event"
_EVENTS_SCHEME = 1
(_EV_ID, _EV_SH_TGT, _EV_SH_START, _EV_SH_TARGET, _EV_SH_RAW,
 _EV_ST_IDX, _EV_ST_ARGC, _EV_ST_RZ, _EV_ST_ARGS,
 _EV_NM_HEAD, _EV_NM_NAME, _EV_LIT,
 _EV_RP_KIND, _EV_RP_HEAD, _EV_RP_TEXT,
 _EV_SP_MODE, _EV_SP_POINT, _EV_SP_RAW,
 _EV_DT_KILLER, _EV_DT_HEAD, _EV_DT_MASK, _EV_DT_DWORDS,
 _EV_FT_HEAD, _EV_FT_SAME, _EV_FT_SITE, _EV_FT_ARGS,
 _EV_FE_ROOT, _EV_FE_RESULT,
 _EV_CREATE, _EV_DRAW, _EV_RAW, _EV_COLS) = range(32)
_EV_DICT, _EV_STARTS, _EV_SPRAY_BYTES = 256, 4, 44
NAME_MAX, TEXT_MAX = 32, 128               # REC_NAME_MAX, REC_TEXT_MAX
DEATHS_MAX = tarec.REC_DEATHS_MAX
UNIT_BITS = tarec.REC_UNIT_BITS            # 11
_ROOT_ARGS = np.array(tarec.ROOT_ARGS, np.int64)
FR_ROOTS = len(tarec.ROOT_ARGS)
NET_CREATE = 32                            # sizeof(RecNetCreate)
# EV_BODY_MAX, the longest event, a death: every write below lands in body[:_EV_BODY_MAX], which
# _events_rec bounds against the body before its first write
_EV_BODY_MAX = 12 + IMG
assert 1 <= _EV_COLS <= FRAME_COLS_MAX
assert max(24 + NAME_MAX, 12 + TEXT_MAX, 8 + int(_ROOT_ARGS.max()), NET_CREATE) <= _EV_BODY_MAX <= BODY_MAX
(RK_EV_CREATE, RK_EV_DEATH, RK_EV_INTERCEPT, RK_EV_START, RK_EV_START_NAMED, RK_EV_BUILT,
 RK_EV_STATE, RK_EV_REPLY, RK_EV_SPRAY, RK_EV_FEATURE, RK_EV_FEATURE_END, RK_EV_DRAW,
 RK_EV_DEATH_END, RK_EV_NET_CREATE) = (
    tarec.RK_EV_CREATE, tarec.RK_EV_DEATH, tarec.RK_EV_INTERCEPT, tarec.RK_EV_START,
    tarec.RK_EV_START_NAMED, tarec.RK_EV_BUILT, tarec.RK_EV_STATE, tarec.RK_EV_REPLY,
    tarec.RK_EV_SPRAY, tarec.RK_EV_FEATURE, tarec.RK_EV_FEATURE_END, tarec.RK_EV_DRAW,
    tarec.RK_EV_DEATH_END, tarec.RK_EV_NET_CREATE)
_M_LAST, _M_NAMES, _M_TEXTS, _M_DEATHS = range(4)


def _events_new(prm):
    """The events frame's state for a usable segment's params."""
    n = prm.nslots
    return (np.zeros(4, np.int64),
            np.zeros((_EV_DICT, NAME_MAX), np.uint8), np.zeros(_EV_DICT, np.int64),
            np.zeros((_EV_DICT, TEXT_MAX), np.uint8), np.zeros(_EV_DICT, np.int64),
            np.zeros((n, _EV_STARTS, 5), np.int64), np.zeros((n, 2), np.int64),
            np.zeros((n, _EV_SPRAY_BYTES), np.uint8), np.zeros(n, np.bool_),
            np.zeros(FR_ROOTS, np.int64), np.zeros(FR_ROOTS, np.bool_),
            np.zeros(DEATHS_MAX, np.int64))


def _events_restart(st):
    for a in st:
        a[...] = 0


def _events_clone(st):
    return tuple(a.copy() for a in st)


@_jit
def _events_open(st, cols, sec, at, end, iters, err):
    return _frame_open(sec, at, end, _EVENTS_SCHEME, _EV_COLS, cols, err, RCS_EVENTS)


@_jit
def _events_iter(st, cols, sec, it, gt, copy, live, err):
    return 0


@_jit
def _le32(a, at):
    return (np.int64(a[at]) | np.int64(a[at + 1]) << 8 | np.int64(a[at + 2]) << 16 |
            np.int64(a[at + 3]) << 24)


@_jit
def _put_le32(a, at, v):
    a[at], a[at + 1], a[at + 2], a[at + 3] = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF


@_jit
def _ev_zz(sec, cols, k, pred, body, at):
    """zz(value - pred) from column k into body[at:at + 4]: False cut short."""
    v, ok = _col_varint(sec, cols, k)
    if ok:
        _put_le32(body, at, (pred + _unzz32(v)) & M32)
    return ok


@_jit
def _ev_id(st, sec, cols):
    """The next ID, which becomes the last: (ID, False) cut short."""
    v, ok = _col_varint(sec, cols, _EV_ID)
    if not ok:
        return np.int64(0), False
    i = (st[0][_M_LAST] + _unzz32(v)) & M32
    st[0][_M_LAST] = i
    return i, True


@_jit
def _ev_live(copy, live, i):
    """The slot of a unit ID that is a live slot of the copy, else -1."""
    if i < 1 or i - 1 >= len(live) or not live[i - 1]:
        return -1
    return i - 1


@_jit
def _ev_args(sec, cols, body, at, argc, starts, s, e):
    """A start's arguments into body[at:at + 16], against slot s's entry e (-1: zero): 1, 0 cut
    short, -1 a bad flag."""
    na = argc if argc < 4 else 4
    rz, ok = _col_u8(sec, cols, _EV_ST_RZ)
    if not ok:
        return 0
    if rz > 1:
        return -1
    body[at:at + 16] = 0
    for i in range(na if rz else 4):
        pred = starts[s, e, 1 + i] if e >= 0 else 0
        if not _ev_zz(sec, cols, _EV_ST_ARGS, pred, body, at + 4 * i):
            return 0
    return 1


@_jit
def _ev_text(sec, cols, k, dtext, dlen, misc, mi, body, at, mx):
    """A name or text into body[at:] from the dictionary or a literal: its length, or -1 cut short,
    -2 past the entries or the bound."""
    v, ok = _col_varint(sec, cols, k)
    if not ok:
        return -1
    if v:
        if v > misc[mi]:
            return -2
        n = dlen[v - 1]
        body[at:at + n] = dtext[v - 1, :n]
        return n
    n, ok = _col_varint(sec, cols, _EV_LIT)
    if not ok:
        return -1
    if n > mx:
        return -2
    if not _col_bytes(sec, cols, _EV_LIT, body, at, n):
        return -1
    if misc[mi] < _EV_DICT:
        dtext[misc[mi], :n] = body[at:at + n]
        dlen[misc[mi]] = n
        misc[mi] += 1
    return n


@_jit
def _events_rec(st, cols, sec, it, gt, events, copy, live, kind, body, err):
    misc, starts, snum, spray, sprayed, site, sited, deaths = st[0], st[5], st[6], st[7], st[8], \
        st[9], st[10], st[11]
    if body.shape[0] < _EV_BODY_MAX:
        return -_fail(err, E_EV_BODY, body.shape[0])
    if kind == RK_EV_SHOT:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        if i > 0xFFFF:
            return -_fail(err, E_EV_SHOOTER, i)
        v, ok = _col_varint(sec, cols, _EV_SH_TGT)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_SH_TGT)
        tg = (i + _unzz32(v - 1)) & M32 if v else 0xFFFF
        if v and tg >= 0xFFFF:
            return -_fail(err, E_EV_TARGET, tg)
        body[:SHOT] = 0
        for w in range(2):
            u, at, k = (i, 1, _EV_SH_START) if w == 0 else (tg, 0xD, _EV_SH_TARGET)
            s = _ev_live(copy, live, u) if u != 0xFFFF else -1
            for q in range(3):
                pred = _le32(copy[s], 0x6A + 4 * q) if s >= 0 else 0
                if not _ev_zz(sec, cols, k, pred, body, at + 4 * q):
                    return -_fail(err, E_EV_CUT, kind, k)
        if not (_col_bytes(sec, cols, _EV_SH_RAW, body, 0, 1) and
                _col_bytes(sec, cols, _EV_SH_RAW, body, 0x19, 6) and
                _col_bytes(sec, cols, _EV_SH_RAW, body, 0x23, 1)):
            return -_fail(err, E_EV_CUT, kind, _EV_SH_RAW)
        body[0x1F], body[0x20], body[0x21], body[0x22] = tg & 0xFF, tg >> 8, i & 0xFF, i >> 8
        return SHOT
    if kind == RK_EV_START:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        v, ok = _col_varint(sec, cols, _EV_ST_IDX)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ST_IDX)
        idx = _unzz32(v)
        v, ok = _col_varint(sec, cols, _EV_ST_ARGC)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ST_ARGC)
        argc = _unzz32(v)
        s = i - 1 if 1 <= i and i - 1 < len(snum) else -1
        e = -1
        if s >= 0:
            for q in range(snum[s, 0]):
                if starts[s, q, 0] == idx:
                    e = q
                    break
        r = _ev_args(sec, cols, body, 12, argc, starts, s, e)
        if r == 0:
            return -_fail(err, E_EV_CUT, kind, _EV_ST_ARGS)
        if r < 0:
            return -_fail(err, E_EV_ARGFLAG, kind)
        _put_le32(body, 0, i)
        _put_le32(body, 4, idx)
        _put_le32(body, 8, argc)
        if s >= 0:
            if e < 0:
                e = snum[s, 1]
                snum[s, 1] = (e + 1) % _EV_STARTS
                if snum[s, 0] < _EV_STARTS:
                    snum[s, 0] += 1
                starts[s, e, 0] = idx
            for q in range(4):
                starts[s, e, 1 + q] = _le32(body, 12 + 4 * q)
        return 28
    if kind == RK_EV_START_NAMED:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        body[:24] = 0
        _put_le32(body, 0, i)
        if not _col_bytes(sec, cols, _EV_NM_HEAD, body, 4, 2):
            return -_fail(err, E_EV_CUT, kind, _EV_NM_HEAD)
        r = _ev_args(sec, cols, body, 8, np.int64(body[4]), starts, 0, -1)
        if r == 0:
            return -_fail(err, E_EV_CUT, kind, _EV_ST_ARGS)
        if r < 0:
            return -_fail(err, E_EV_ARGFLAG, kind)
        n = _ev_text(sec, cols, _EV_NM_NAME, st[1], st[2], misc, _M_NAMES, body, 24, NAME_MAX)
        if n == -1:
            return -_fail(err, E_EV_CUT, kind, _EV_NM_NAME)
        if n < 0:
            return -_fail(err, E_EV_TEXT, kind)
        return 24 + n
    if kind == RK_EV_REPLY:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        body[:12] = 0
        _put_le32(body, 0, i)
        v, ok = _col_varint(sec, cols, _EV_RP_KIND)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_RP_KIND)
        _put_le32(body, 4, v)
        if not _col_bytes(sec, cols, _EV_RP_HEAD, body, 8, 2):
            return -_fail(err, E_EV_CUT, kind, _EV_RP_HEAD)
        n = _ev_text(sec, cols, _EV_RP_TEXT, st[3], st[4], misc, _M_TEXTS, body, 12, TEXT_MAX)
        if n == -1:
            return -_fail(err, E_EV_CUT, kind, _EV_RP_TEXT)
        if n < 0:
            return -_fail(err, E_EV_TEXT, kind)
        return 12 + n
    if kind == RK_EV_SPRAY:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        _put_le32(body, 0, i)
        s = i - 1 if 1 <= i and i - 1 < len(sprayed) else -1
        mode, ok = _col_u8(sec, cols, _EV_SP_MODE)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_SP_MODE)
        if mode > 2 or (mode < 2 and not (s >= 0 and sprayed[s])):
            return -_fail(err, E_EV_SPRAY, mode, i)
        if mode == 2:
            if not _col_bytes(sec, cols, _EV_SP_RAW, body, 4, _EV_SPRAY_BYTES):
                return -_fail(err, E_EV_CUT, kind, _EV_SP_RAW)
        else:
            body[4:4 + _EV_SPRAY_BYTES] = spray[s]
            if mode == 1:
                for q in range(3):
                    if not _ev_zz(sec, cols, _EV_SP_POINT, _le32(spray[s], 4 + 4 * q), body,
                                  8 + 4 * q):
                        return -_fail(err, E_EV_CUT, kind, _EV_SP_POINT)
        if s >= 0:
            spray[s, :] = body[4:4 + _EV_SPRAY_BYTES]
            sprayed[s] = True
        return 48
    if kind == RK_EV_DEATH:
        if misc[_M_DEATHS] >= DEATHS_MAX:
            return -_fail(err, E_EV_NEST, DEATHS_MAX)
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        deaths[misc[_M_DEATHS]] = i
        misc[_M_DEATHS] += 1
        _put_le32(body, 0, i)
        if not _ev_zz(sec, cols, _EV_DT_KILLER, i, body, 4):
            return -_fail(err, E_EV_CUT, kind, _EV_DT_KILLER)
        if not _col_bytes(sec, cols, _EV_DT_HEAD, body, 8, 4):
            return -_fail(err, E_EV_CUT, kind, _EV_DT_HEAD)
        mask = np.zeros(UNIT_BITS, np.uint8)
        if not _col_bytes(sec, cols, _EV_DT_MASK, mask, 0, UNIT_BITS):
            return -_fail(err, E_EV_CUT, kind, _EV_DT_MASK)
        for q in range(NDW, 8 * UNIT_BITS):
            if mask[q >> 3] >> (q & 7) & 1:
                return -_fail(err, E_EV_MASK, q)
        s = _ev_live(copy, live, i)
        if s >= 0:
            body[12:12 + IMG] = copy[s]
        else:
            body[12:12 + IMG] = 0
        for q in range(NDW):
            if mask[q >> 3] >> (q & 7) & 1:
                if not _col_bytes(sec, cols, _EV_DT_DWORDS, body, 12 + 4 * q, 4):
                    return -_fail(err, E_EV_CUT, kind, _EV_DT_DWORDS)
        return 12 + IMG
    if kind == RK_EV_DEATH_END:
        if misc[_M_DEATHS] == 0:
            return -_fail(err, E_EV_NODEATH)
        misc[_M_DEATHS] -= 1
        _put_le32(body, 0, deaths[misc[_M_DEATHS]])
        return 4
    if kind == RK_EV_FEATURE:
        if not _col_bytes(sec, cols, _EV_FT_HEAD, body, 0, 2):
            return -_fail(err, E_EV_CUT, kind, _EV_FT_HEAD)
        root = np.int64(body[0])
        if root >= FR_ROOTS:
            return -_fail(err, E_EV_ROOT, root)
        body[2], body[3] = 0, 0
        same, ok = _col_u8(sec, cols, _EV_FT_SAME)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_FT_SAME)
        if same > 1 or (same and not sited[root]):
            return -_fail(err, E_EV_SAME, root)
        if same:
            _put_le32(body, 4, site[root])
        elif not _col_bytes(sec, cols, _EV_FT_SITE, body, 4, 4):
            return -_fail(err, E_EV_CUT, kind, _EV_FT_SITE)
        site[root] = _le32(body, 4)
        sited[root] = True
        n = _ROOT_ARGS[root]
        if not _col_bytes(sec, cols, _EV_FT_ARGS, body, 8, n):
            return -_fail(err, E_EV_CUT, kind, _EV_FT_ARGS)
        return 8 + n
    if kind == RK_EV_FEATURE_END:
        body[:8] = 0
        if not _col_bytes(sec, cols, _EV_FE_ROOT, body, 0, 1):
            return -_fail(err, E_EV_CUT, kind, _EV_FE_ROOT)
        v, ok = _col_varint(sec, cols, _EV_FE_RESULT)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_FE_RESULT)
        _put_le32(body, 4, v)
        return 8
    if kind == RK_EV_CREATE or kind == RK_EV_NET_CREATE:
        n, raw = (28, 21) if kind == RK_EV_CREATE else (NET_CREATE, 26)
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        body[:n] = 0
        _put_le32(body, 0, i)
        if not _col_bytes(sec, cols, _EV_CREATE, body, 4, raw):
            return -_fail(err, E_EV_CUT, kind, _EV_CREATE)
        return n
    if kind == RK_EV_DRAW:
        body[:12] = 0
        if not (_col_bytes(sec, cols, _EV_DRAW, body, 0, 1) and
                _col_bytes(sec, cols, _EV_DRAW, body, 4, 8)):
            return -_fail(err, E_EV_CUT, kind, _EV_DRAW)
        return 12
    if kind == RK_EV_BUILT or kind == RK_EV_STATE:
        i, ok = _ev_id(st, sec, cols)
        if not ok:
            return -_fail(err, E_EV_CUT, kind, _EV_ID)
        body[:8] = 0
        _put_le32(body, 0, i)
        if not _col_bytes(sec, cols, _EV_RAW, body, 4, 4 if kind == RK_EV_BUILT else 1):
            return -_fail(err, E_EV_CUT, kind, _EV_RAW)
        return 8
    if kind == RK_EV_INTERCEPT:
        body[:16] = 0
        if not _col_bytes(sec, cols, _EV_RAW, body, 0, 14):
            return -_fail(err, E_EV_CUT, kind, _EV_RAW)
        return 16
    return -_fail(err, E_EV_KIND, kind)


@_jit
def _events_iter_end(st, cols, sec, it, gt, copy, live, err):
    return 0


@_jit
def _events_close(st, cols, sec, err):
    return _frame_close(cols, _EV_COLS, err, RCS_EVENTS)


# ---- THE STATE FRAME (tagpu_recframe_state.c) ----------------------------------------------------
#
# Scheme 1, nineteen columns: the globals, the players, the wreck records and RK_FEATURES each
# against what the frame last saw of them (the C file says what each column holds). The state, a
# tuple of arrays, is the C's: the globals with known bits, last deltas and the last delta's mask;
# each player's image, known bits, last deltas and last mask; the wreck records with known bits;
# RK_FEATURES' fields; the iteration's method-1 count and next wreck index. The last array, the
# grid's cells, is the segment's and survives a restart.

(E_STATE_KIND, E_STATE_CUT, E_STATE_GCOUNT, E_STATE_GMASK, E_STATE_GSAME, E_STATE_METH,
 E_STATE_UNKNOWN, E_STATE_STEPS, E_STATE_PM_EACH, E_STATE_PLAYER, E_STATE_PFLAG, E_STATE_PBITS,
 E_STATE_WRECK, E_STATE_FMASK, E_STATE_BODY) = range(250, 265)
_WHY.update({E_STATE_KIND: "kind {0} in the state frame",
             E_STATE_CUT: "the state frame's column {0} cut short",
             E_STATE_GCOUNT: "a globals image of {0} dwords, not 1..256",
             E_STATE_GMASK: "a globals mask of {0} bytes",
             E_STATE_GSAME: "a globals delta repeats a mask before the first",
             E_STATE_METH: "global {0}'s method {1}",
             E_STATE_UNKNOWN: "global {0} is unknown under method {1}",
             E_STATE_STEPS: "global {0} takes {1} steps, not 1..16384",
             E_STATE_PM_EACH: "more than 2 globals an iteration by steps",
             E_STATE_PLAYER: "player {0} in the state frame",
             E_STATE_PFLAG: "a player mask flag {0}",
             E_STATE_PBITS: "player {0}'s mask names dword {1}",
             E_STATE_WRECK: "wreck record {0} past the pool's {1}",
             E_STATE_FMASK: "a feature mask {0}",
             E_STATE_BODY: "a state record's body past 0xFFFF bytes"})
_STATE_SCHEME, _STATE_COLS = 1, 19
(_SC_GFULL, _SC_GM, _SC_GMASK, _SC_GMETH, _SC_GVAL, _SC_GSTEPS, _SC_GRAW, _SC_PL, _SC_PM, _SC_PMASK,
 _SC_PIMG, _SC_PVAL, _SC_PRAW, _SC_WI, _SC_WM, _SC_WV, _SC_WRAW, _SC_FM, _SC_FV) = range(_STATE_COLS)
assert 1 <= _STATE_COLS <= FRAME_COLS_MAX
_GLOBALS_MAX, _PM_CAP, _PM_EACH, _PM_M = 256, 16384, 2, 0x7FFFFFFF
_RK_GLOBALS, _RK_GLOBALS_DELTA = tarec.RK_GLOBALS, tarec.RK_GLOBALS_DELTA
_RK_PLAYER, _RK_PLAYER_DELTA = tarec.RK_PLAYER, tarec.RK_PLAYER_DELTA
_RK_WRECK, _RK_FEATURES = tarec.RK_WRECK, tarec.RK_FEATURES
_PLAYERS, _P_IMG, _P_DW = tarec.REC_PLAYERS, tarec.REC_PLAYER_IMAGE, tarec.REC_PLAYER_DWORDS
_P_BITS, _P_HEAD = tarec.REC_PLAYER_BITS, tarec.REC_PLAYER_DELTA_HEAD
_WRECK = tarec.REC_WRECK_BYTES


def _state_new(prm):
    """The state frame's state for a usable segment's params."""
    return (np.zeros(_GLOBALS_MAX, np.int64), np.zeros(_GLOBALS_MAX, np.int64),
            np.zeros(_GLOBALS_MAX, np.uint8), np.zeros(_GLOBALS_MAX // 8, np.uint8),
            np.zeros((_PLAYERS, _P_DW), np.int64), np.zeros((_PLAYERS, _P_DW), np.int64),
            np.zeros((_PLAYERS, _P_DW), np.uint8), np.zeros((_PLAYERS, _P_BITS), np.uint8),
            np.zeros((prm.nwrecks, _WRECK), np.uint8), np.zeros(prm.nwrecks, np.uint8),
            np.zeros(4, np.int64), np.zeros(3, np.int64), np.array([prm.gw * prm.gh], np.int64))


def _state_restart(st):
    for a in st[:-1]:
        a[...] = 0


def _state_clone(st):
    return tuple(a.copy() for a in st)


@_jit
def _state_open(st, cols, sec, at, end, iters, err):
    return _frame_open(sec, at, end, _STATE_SCHEME, _STATE_COLS, cols, err, RCS_STATE)


@_jit
def _state_iter(st, cols, sec, it, gt, copy, live, err):
    meta = st[11]
    meta[1] = 0
    meta[2] = 0
    return 0


@_inline
def _st_get32(b, at):
    return (np.int64(b[at]) | np.int64(b[at + 1]) << 8 | np.int64(b[at + 2]) << 16 |
            np.int64(b[at + 3]) << 24)


@_inline
def _st_put32(b, at, v):
    b[at], b[at + 1], b[at + 2], b[at + 3] = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF


@_jit
def _st_u32(sec, cols, k):
    """The next 4 bytes of column k as a u32: (value, True), or (0, False) past its end."""
    at = cols[k, 0]
    if cols[k, 1] - at < 4:
        return np.int64(0), False
    cols[k, 0] = at + 4
    return _st_get32(sec, at), True


@_inline
def _st_pm(s):
    t = (s % _PM_M) * 16807 % _PM_M
    return t if t else np.int64(_PM_M)


@_jit
def _st_global(st, cols, sec, d, err):
    """Changed global d's value from its method: the value, or -code."""
    g, gd, gk, meta = st[0], st[1], st[2], st[11]
    meth, ok = _col_u8(sec, cols, _SC_GMETH)
    if not ok:
        return -_fail(err, E_STATE_CUT, _SC_GMETH)
    if meth > 2:
        return -_fail(err, E_STATE_METH, d, meth)
    if meth < 2 and not gk[d]:
        return -_fail(err, E_STATE_UNKNOWN, d, meth)
    if meth == 0:
        z, ok = _col_varint(sec, cols, _SC_GVAL)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_GVAL)
        v = (g[d] + gd[d] + _unzz32(z)) & M32
    elif meth == 1:
        z, ok = _col_varint(sec, cols, _SC_GSTEPS)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_GSTEPS)
        if z < 1 or z > _PM_CAP:
            return -_fail(err, E_STATE_STEPS, d, z)
        if meta[1] >= _PM_EACH:
            return -_fail(err, E_STATE_PM_EACH)
        meta[1] += 1
        v = g[d]
        for _ in range(z):
            v = _st_pm(v)
    else:
        v, ok = _st_u32(sec, cols, _SC_GRAW)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_GRAW)
    gd[d] = (v - g[d]) & M32 if gk[d] else 0
    g[d] = v
    gk[d] = 1
    return v


@_jit
def _state_rec(st, cols, sec, it, gt, events, copy, live, kind, body, err):
    g, gk, gm, meta = st[0], st[2], st[3], st[11]
    if kind == _RK_GLOBALS:
        n, ok = _col_varint(sec, cols, _SC_GFULL)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_GFULL)
        if n < 1 or n > _GLOBALS_MAX:
            return -_fail(err, E_STATE_GCOUNT, n)
        if not _col_bytes(sec, cols, _SC_GFULL, body, 0, 4 * n):
            return -_fail(err, E_STATE_CUT, _SC_GFULL)
        for d in range(_GLOBALS_MAX):
            g[d] = _st_get32(body, 4 * d) if d < n else 0
            gk[d] = d < n
        return 4 * n
    if kind == _RK_GLOBALS_DELTA:
        m, ok = _col_u8(sec, cols, _SC_GM)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_GM)
        if m > _GLOBALS_MAX // 8:
            return -_fail(err, E_STATE_GMASK, m)
        if m == 0:
            if meta[0] == 0:
                return -_fail(err, E_STATE_GSAME)
        else:
            if not _col_bytes(sec, cols, _SC_GMASK, gm, 0, m):
                return -_fail(err, E_STATE_CUT, _SC_GMASK)
            meta[0] = m
        nb = meta[0]
        head = (nb + 3) & ~3
        if head > BODY_MAX:
            return -_fail(err, E_STATE_BODY)
        body[:nb] = gm[:nb]
        body[nb:head] = 0
        ln = head
        for d in range(8 * nb):
            if gm[d >> 3] >> (d & 7) & 1:
                v = _st_global(st, cols, sec, d, err)
                if v < 0:
                    return v
                if ln + 4 > BODY_MAX:
                    return -_fail(err, E_STATE_BODY)
                _st_put32(body, ln, v)
                ln += 4
        return ln
    if kind == _RK_PLAYER:
        pv, pk = st[4], st[6]
        p, ok = _col_u8(sec, cols, _SC_PL)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_PL)
        if p >= _PLAYERS:
            return -_fail(err, E_STATE_PLAYER, p)
        if not _col_bytes(sec, cols, _SC_PIMG, body, 4, _P_IMG):
            return -_fail(err, E_STATE_CUT, _SC_PIMG)
        if 4 > BODY_MAX:
            return -_fail(err, E_STATE_BODY)
        body[0], body[1], body[2], body[3] = p, 0, 0, 0
        for i in range(_P_DW):
            pv[p, i] = _st_get32(body, 4 + 4 * i)
            pk[p, i] = 1
        return 4 + _P_IMG
    if kind == _RK_PLAYER_DELTA:
        pv, pd, pk, pmk = st[4], st[5], st[6], st[7]
        p, ok = _col_u8(sec, cols, _SC_PL)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_PL)
        if p >= _PLAYERS:
            return -_fail(err, E_STATE_PLAYER, p)
        same, ok = _col_u8(sec, cols, _SC_PM)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_PM)
        if same > 1:
            return -_fail(err, E_STATE_PFLAG, same)
        mask = pmk[p]
        if not same:
            if not _col_bytes(sec, cols, _SC_PMASK, mask, 0, _P_BITS):
                return -_fail(err, E_STATE_CUT, _SC_PMASK)
            for i in range(_P_DW, 8 * _P_BITS):
                if mask[i >> 3] >> (i & 7) & 1:
                    return -_fail(err, E_STATE_PBITS, p, i)
        if _P_HEAD > BODY_MAX:
            return -_fail(err, E_STATE_BODY)
        body[0], body[1] = p, 0
        body[2:2 + _P_BITS] = mask
        body[_P_HEAD - 1] = 0
        ln = _P_HEAD
        for i in range(_P_DW):
            if mask[i >> 3] >> (i & 7) & 1:
                if pk[p, i]:
                    z, ok = _col_varint(sec, cols, _SC_PVAL)
                    if not ok:
                        return -_fail(err, E_STATE_CUT, _SC_PVAL)
                    v = (pv[p, i] + pd[p, i] + _unzz32(z)) & M32
                    pd[p, i] = (v - pv[p, i]) & M32
                else:
                    v, ok = _st_u32(sec, cols, _SC_PRAW)
                    if not ok:
                        return -_fail(err, E_STATE_CUT, _SC_PRAW)
                    pd[p, i] = 0
                pv[p, i] = v
                pk[p, i] = 1
                if ln + 4 > BODY_MAX:
                    return -_fail(err, E_STATE_BODY)
                _st_put32(body, ln, v)
                ln += 4
        return ln
    if kind == _RK_WRECK:
        w, wk = st[8], st[9]
        z, ok = _col_varint(sec, cols, _SC_WI)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_WI)
        i = (meta[2] + _unzz32(z)) & M32
        if i >= len(wk):
            return -_fail(err, E_STATE_WRECK, i, len(wk))
        meta[2] = i + 1
        rec = w[i]
        if wk[i]:
            if cols[_SC_WM, 1] - cols[_SC_WM, 0] < 3:
                return -_fail(err, E_STATE_CUT, _SC_WM)
            a = cols[_SC_WM, 0]
            m = np.int64(sec[a]) | np.int64(sec[a + 1]) << 8 | np.int64(sec[a + 2]) << 16
            cols[_SC_WM, 0] = a + 3
            for j in range(_WRECK // 2):
                if m >> j & 1 and not _col_bytes(sec, cols, _SC_WV, rec, 2 * j, 2):
                    return -_fail(err, E_STATE_CUT, _SC_WV)
        else:
            if not _col_bytes(sec, cols, _SC_WRAW, rec, 0, _WRECK):
                return -_fail(err, E_STATE_CUT, _SC_WRAW)
            wk[i] = 1
        if 4 + _WRECK > BODY_MAX:
            return -_fail(err, E_STATE_BODY)
        body[0], body[1], body[2], body[3] = i & 0xFF, i >> 8, 0, 0
        body[4:4 + _WRECK] = rec
        return 4 + _WRECK
    if kind == _RK_FEATURES:
        fv, cells = st[10], st[12][0]
        m, ok = _col_u8(sec, cols, _SC_FM)
        if not ok:
            return -_fail(err, E_STATE_CUT, _SC_FM)
        if m > 15:
            return -_fail(err, E_STATE_FMASK, m)
        if 16 > BODY_MAX:
            return -_fail(err, E_STATE_BODY)
        for j in range(4):
            if j < 3:
                v = fv[j]
            else:
                v = fv[3] - 1 if fv[3] else cells - 1
            if m >> j & 1:
                z, ok = _col_varint(sec, cols, _SC_FV)
                if not ok:
                    return -_fail(err, E_STATE_CUT, _SC_FV)
                v = (v + _unzz32(z)) & M32
            fv[j] = v
            _st_put32(body, 4 * j, v)
        return 16
    return -_fail(err, E_STATE_KIND, kind)


@_jit
def _state_iter_end(st, cols, sec, it, gt, copy, live, err):
    return 0


@_jit
def _state_close(st, cols, sec, err):
    return _frame_close(cols, _STATE_COLS, err, RCS_STATE)

def _frames_new(prm):
    """The record frames' states, cells, orders, events and state, for a usable segment's params."""
    return (_cells_new(prm), _orders_new(prm), _events_new(prm), _state_new(prm))


def _frames_restart(fst):
    _cells_restart(fst[0])
    _orders_restart(fst[1])
    _events_restart(fst[2])
    _state_restart(fst[3])


def _frames_clone(fst):
    return (_cells_clone(fst[0]), _orders_clone(fst[1]), _events_clone(fst[2]),
            _state_clone(fst[3]))


class Frame:
    """One reading's frames. The unit frame: the copy of the units and its lanes (L1), L2, L3's
    five lanes, the winners, the history's age and the resting flags, for a segment's params. l1
    is the copy cut into lanes for every live slot between iterations: a restart and a slot's
    RK_UNIT cut it again, and an advance moves both on together. `rest=False` evaluates every
    candidate every iteration, which must give the same bytes (the fixed-point argument). The
    pool frame: the copy of the projectile pool (zero from its extent on), each record's image
    one iteration before, its winners and its age, the last birth of each weapon ID, the last
    iteration's shots, and meta = (count, extent, shots). The record frames: `fst`, their states,
    made for the sizes the segment's params give (`sizes`), which every restart's must match, as
    reccodec_restart requires; None while the params are not usable."""

    def __init__(self, prm, rest=True):
        self.prm = prm
        self.rest_mode = rest
        self.sizes = (prm.nslots, prm.nproj, prm.gw, prm.gh, prm.nwrecks)
        self.fst = _frames_new(prm) if prm.usable else None
        n = prm.nslots
        self.copy = np.zeros((n, IMG), np.uint8)
        self.live = np.zeros(n, bool)
        n = prm.nproj
        self.P = np.zeros((n, PDW), np.int64)
        self.H = np.zeros((n, PDW), np.int64)
        self.pwin = np.zeros((n, PDW), np.int8)
        self.page = np.zeros(n, np.int8)
        self.lastnew = np.zeros((WEAPON_IDS + 1, PDW), np.int64)
        self.shots = np.zeros((SHOTS_MAX, SHOT), np.uint8)
        self.meta = np.zeros(3, np.int64)
        self.valid = False
        self._derive()

    def clone(self):
        f = Frame.__new__(Frame)
        f.__dict__.update(self.__dict__)
        for k in ("copy", "live", "l1", "l2", "l3", "win", "age", "rest", "lstep", "lset", "P", "H", "pwin", "page",
                  "lastnew", "shots", "meta"):
            if hasattr(self, k):
                setattr(f, k, getattr(self, k).copy())
        if self.fst is not None:
            f.fst = _frames_clone(self.fst)
        return f

    def _derive(self):
        """The segment's lane table and constants as the kernels take them, `tab`: off, width,
        the lane masks (2^(8 width) - 1), J (the lane of each _RULED role), the default winners,
        S1's and S2's lanes, the COUNT lanes, the aircraft index by type, the aircraft rows,
        (q, sea, sight) and JL (the lane of each _LEDGER role)."""
        p = self.prm
        self.nl = len(p.lanes)
        if not self.nl:
            return
        off = np.array([o for o, _, _ in p.lanes], np.int64)
        width = np.array([w for _, w, _ in p.lanes], np.int64)
        role = [r for _, _, r in p.lanes]
        lane = {r: j for j, r in enumerate(role) if r not in (ROLE["PLAIN"], ROLE["COUNT"])}
        J = np.array([lane[ROLE[name]] for name in _RULED], np.int64)
        JL = np.array([lane[ROLE[name]] for name in _LEDGER], np.int64)
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
                    np.array([q, p.sea, p.sight], np.int64), JL)
        self.l3_lanes = J[_L3]

    def restart(self, prm=None):
        """Every frame restarts: the unit and pool frames from this reading's own copies of the
        units and the pool, the cells frame from this reading's own grid (every image it has read,
        a records block's too), the other record frames empty."""
        _compile()
        if prm is not None:
            self.prm = prm
            self._derive()
        self.valid = False
        p = self.prm
        if not p.usable or not self.nl or (p.nslots, p.nproj, p.gw, p.gh, p.nwrecks) != self.sizes:
            return
        if self.fst is None:
            self.fst = _frames_new(p)
        _frames_restart(self.fst)
        c, h = int(self.meta[0]), int(self.meta[1])
        if c > h or h > len(self.P):
            return
        self.P[h:] = 0
        self.pwin[:] = 0
        self.page[:] = 0
        self.page[:c] = 1
        self.lastnew[:] = 0
        self.meta[2] = 0
        n = self.prm.nslots
        self.copy[~self.live] = 0
        self.l1 = _split(self.copy, self.tab[0], self.tab[1])
        self.l2 = self.l1.copy()
        self.l3 = self.l1[:, self.l3_lanes]
        self.win = np.tile(self.tab[4], (n, 1))
        self.age = self.live.astype(np.int8)
        self.rest = np.zeros(n, bool)
        self.lstep = np.zeros((n, 2), np.int64)
        self.lset = np.zeros((OWNERS, 2), np.int64)
        self.valid = True

    def invalidate(self):
        self.valid = False

    def apply_records(self, payload):
        """A records block's RK_UNIT, RK_UNIT_DELTA and RK_UNIT_GONE by the writer's rules, its
        RK_PROJ_HEAD, RK_PROJ and RK_PROJ_DELTA by the reader's, and its images onto the cells
        frame's grid by the reader's rule: an RK_CELLS with no RCS_EVENTS record before it in its
        iteration."""
        at, n = 0, len(payload)
        events = False
        while n - at >= 4:
            kind, ln = struct.unpack_from("<HH", payload, at)
            rl = (4 + ln + 3) & ~3
            if rl > n - at:
                break
            b = at + 4
            if kind == RK_TICK:
                events = False
            elif kind == tarec.RK_CELLS and not events and self.fst is not None:
                _cells_image(self.fst[0], payload, b, ln)
            elif section_of(kind) == RCS_EVENTS:
                events = True
            if kind in (RK_PROJ_HEAD, RK_PROJ, RK_PROJ_DELTA):
                self._apply_proj(kind, payload, b, ln)
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

    def _apply_proj(self, kind, payload, b, ln):
        """A projectile record by tagpu_recreader.c's rules; one it refuses is skipped."""
        c, h = int(self.meta[0]), int(self.meta[1])
        if kind == RK_PROJ_HEAD:
            if ln == 8:
                v0, v1 = struct.unpack_from("<II", payload, b)
                if v0 <= v1 <= len(self.P):
                    self.P[v1:h] = 0
                    self.meta[0], self.meta[1] = v0, v1
            return
        if ln < 2:
            return
        i = struct.unpack_from("<H", payload, b)[0]
        if i >= h:
            return
        if kind == RK_PROJ:
            if ln == 4 + PIMG and payload[b + 2] == 0 and payload[b + 3] == 0:
                self.P[i] = np.frombuffer(payload, "<u4", PDW, b + 4)
        elif ln >= 8:
            bits = struct.unpack_from("<I", payload, b + 2)[0]
            sel = [d for d in range(32) if bits >> d & 1]
            if (not sel or sel[-1] < PDW) and ln == 8 + 4 * len(sel):
                self.P[i, sel] = np.frombuffer(payload, "<u4", len(sel), b + 8)

    def decode(self, flags, body):
        """A stream block's bytes (unpacked) to its records."""
        if flags & ~(RBF_STORED | RBF_SECTIONS | RBF_FRAME_RESET | RBF_REFRESH):
            raise Unreadable(f"block flags {flags:#x}")
        if flags & RBF_FRAME_RESET and not flags & RBF_SECTIONS:
            raise Unreadable("RBF_FRAME_RESET without RBF_SECTIONS")
        if not flags & RBF_SECTIONS:
            if self.prm.usable:             # the copy feeds only a restart, which needs the params
                self.apply_records(body)
            self.invalidate()
            return bytes(body)
        if not self.prm.usable:
            raise Unreadable("a sectioned block in a segment without RT_CODEC and RT_PHYSICS, or "
                             "with a grid or a wreck pool past the codec's bounds")
        if flags & RBF_FRAME_RESET:
            self.restart()
        if not self.valid:
            raise Unreadable("a stream block that continues a frame this reading did not start")
        self.valid = False
        out = self._decode(np.frombuffer(body, np.uint8))
        self.valid = True
        return out

    # -- the container --------------------------------------------------------------------------
    def _decode(self, sec):
        _compile()
        n = len(sec)
        if n < SEC_HEAD:
            raise Unreadable(f"a sectioned block of {n} bytes, under its {SEC_HEAD}-byte head")
        payload, iters, nsec = struct.unpack_from("<IHH", sec)
        lens = struct.unpack_from(f"<{RCS_COUNT}I", sec, 8)
        if nsec != RCS_COUNT:
            raise Unreadable(f"{nsec} sections")
        off = [SEC_HEAD]
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
        # RCS_OTHER's bodies: its (length, bytes) entries in order, as many as its kinds hold
        secs = _SECTION_OF[kinds]
        foff = np.array(off, np.int64)
        rec_at, rec_ln = np.zeros((2, len(kinds)), np.int64)
        if _bodies(sec, foff, secs, rec_at, rec_ln, err):
            _refuse(err)
        listed, slots, R = self._units_parse(sec, off[RCS_UNITS], off[RCS_UNITS + 1], iters)
        ctl, codes, S, B, T = self._pool(sec, off[RCS_PROJ], off[RCS_PROJ + 1], iters, err)
        out = np.zeros(payload, np.uint8)
        st = (self.copy, self.l1, self.l2, self.l3, self.win, self.age, self.rest, self.lstep,
              self.lset)
        pl = (ctl, codes, S, B, T, self.P, self.H, self.pwin, self.page, self.lastnew, self.shots,
              self.meta, payload)
        if _iterations(sec, kinds, secs, rec_at, rec_ln, listed, slots, R, self.tab, self.live, st,
                       self.rest_mode, pl, self.fst, foff, out, err):
            _refuse(err)
        return out.tobytes()

    def _pool(self, sec, at, end, iters, err):
        """The pool frame's section, bounded: its control values per iteration (ctl), its codes
        and the survivors', births' and tail slots' residuals as rows (S, B, T), which
        _iterations decodes an iteration at a time (_pool_iter)."""
        ctl = np.zeros((iters, 5), np.int64)
        at, ns, nb, nt = _pool_ctl(sec, at, end, iters, len(self.P), self.meta[0], self.meta[1],
                                   ctl, err)
        if at < 0:
            _refuse(err)
        need = nb + nt + PDW * (ns + nb + nt)
        if need > end - at:
            raise Unreadable(f"the pool frame's codes and columns need {need} bytes, {end - at} are left")
        r = _varints(sec, at, end, need)
        if r is None:
            raise Unreadable("the pool frame's codes or columns cut short")
        vals, at = r
        if at != end:
            raise Unreadable(f"{end - at} bytes left in the pool frame")
        codes, k = vals[:nb + nt], nb + nt
        res = ((vals >> 1) ^ -(vals & 1)) & M32
        S = np.empty((ns, PDW), np.int64)
        S[:, _PORDER] = res[k:k + PDW * ns].reshape(PDW, ns).T
        k += PDW * ns
        B = np.ascontiguousarray(res[k:k + PDW * nb].reshape(PDW, nb).T)
        k += PDW * nb
        T = np.ascontiguousarray(res[k:].reshape(PDW, nt).T)
        return ctl, codes, S, B, T

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


def header_features(es):
    """The grid's columns and rows and the wreck pool's records a header's entries name
    (RT_FEATURES' first three), zeros when it names none."""
    f = [struct.unpack_from("<III", b) for t, b in es if t == tarec.RT_FEATURES and len(b) == 20]
    return f[0] if f else (0, 0, 0)


def header_proj(es):
    """The projectile pool's records a header's entries name (RT_PROJECTILES), 0 when none."""
    n = [struct.unpack("<I", b)[0] for t, b in es if t == tarec.RT_PROJECTILES and len(b) == 4]
    return n[0] if n else 0


def iter_blocks(path, decode=True, rest=True, before_stream=None, data=None, start=None):
    """Every complete block of a recording of format 13, in order: Block(ordinal (from 1), offset,
    kind, flags, first, last, raw, packed, payload) with a stream block's payload its records.
    With decode=False, stream, snapshot and minimap payloads are None. Returns (as a generator's
    value) why the reading stopped short, or None at a clean end; raises Unreadable at a block it
    refuses. `before_stream(ordinal, flags, bytes, frame)` sees each stream block's unpacked bytes
    and the frame it is about to be decoded with (the codec's differential test). `data` is the
    file's bytes when the caller has read them already (reccache.py hashes the very bytes it
    decodes); `path` then names them in the messages.

    `start` (offset, ordinal, root_slots) resumes a walk at an RB_HEADER block: the blocks after
    it are what a walk from the file's start yields, given the ordinal of the block before it and
    the slots of the file's first header (None before any). A header starts new frames, so
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
        allowed = RBF_STORED | (RBF_SECTIONS | RBF_FRAME_RESET | RBF_REFRESH
                                if kind == tarec.RB_STREAM else 0)
        if (flags & ~allowed or (flags & RBF_FRAME_RESET and not flags & RBF_SECTIONS) or
                (kind in (tarec.RB_HEADER, tarec.RB_END, tarec.RB_TIMELINES) and not flags & RBF_STORED)):
            raise Unreadable(f"block flags {flags:#x} on kind {kind} at byte {at}")
        if not flags & RBF_STORED and (not packed or not raw):
            raise Unreadable(f"a packed body of no bytes at byte {at}")
        if flags & RBF_STORED and raw != packed:
            raise Unreadable(f"a stored block of {packed} bytes, not {raw}, at byte {at}")
        ordinal += 1
        lazy = not decode and kind in (tarec.RB_STREAM, tarec.RB_SNAPSHOT, tarec.RB_MINIMAP,
                                       tarec.RB_EXACT)
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
                    prm = params(es, nslots, header_proj(es), *header_features(es))
                else:
                    prm = Params(nslots, header_proj(es), *header_features(es), [], 0, 0, 0, [],
                                 False)
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
