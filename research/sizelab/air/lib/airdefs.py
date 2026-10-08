"""airdefs.py -- the air-war aircraft's def constants, from rev31.gp3's FBIs, as the engine parses
them: 16.16 values are trunc(atof(s) * 65536) (0x4C4800: atof 0x4E4560, x 65536.0, ftol 0x4E43A0),
integers by 0x4C46C0.  def +0x192 maxvelocity, +0x19A brakerate, +0x19E acceleration,
+0x1A2 bankscale (default 1.0), +0x1A6 pitchscale (default 0), +0x1BA turnrate (word),
+0x21C cruisealt (word)   (DISASSEMBLED 0x42C129..0x42C243, 0x42C590..0x42C5AF)."""
import numpy as np
from sintab import T as _T

SIN = _T.astype(np.int64)
FX = lambda s: int(float(s) * 65536)
#             maxv   brake  acc    turn cruise bank
RAW = {26: ("6.6", "4", "0.16", 800, 60, "1"),       # ARMBRAWL
       51: ("10", "6", "0.35", 512, 110, "1"),       # ARMFIG
       123: ("9", "0.4", "0.08", 356, 200, "1"),     # ARMTHUND
       183: ("11", "10", "0.4", 700, 200, "1"),      # CORFINK
       241: ("8", "1.2", "0.07", 335, 205, "1"),     # CORSHAD (no BankScale: the default)
       271: ("12", "7", "0.35", 620, 160, "1")}      # CORVAMP
DEF = {t: dict(maxv=FX(a), brake=FX(b), acc=FX(c), turn=d, cruise=e, bank=FX(f), pitch=0)
       for t, (a, b, c, d, e, f) in RAW.items()}
GRAVITY = 8155            # main+0x14263 on Two Continents, 16.16 (fitted: 112/900 wu/tick^2)
QG = (GRAVITY << 16) // 0xCCD


def per_type(typ, key):
    out = np.zeros(len(typ), np.int64)
    for t, d in DEF.items():
        out[typ == t] = d[key]
    return out


def sinmul(h, s):
    """0x4B70EF: (T[((h+0x20)>>7)&511] * s + 0x1000) >> 13, low dword."""
    idx = ((h.astype(np.int64) + 0x20) >> 7) & 511
    r = (SIN[idx] * s + 0x1000) >> 13
    return _s32(r)


def cosmul(h, s):
    """0x4B7123: the same with h + 0x4020."""
    idx = ((h.astype(np.int64) + 0x4020) >> 7) & 511
    r = (SIN[idx] * s + 0x1000) >> 13
    return _s32(r)


def _s32(a):
    a = np.asarray(a, np.int64) & 0xFFFFFFFF
    return np.where(a >= 1 << 31, a - (1 << 32), a)

# def +0x170 (the sight emit's height term, 0x4825DE), fitted per type on the air war: exact for the
# ARM types, 91-97 % for the CORE ones (air.md); the writer would copy the def's byte.
for _t, _h in {26: 10, 51: 10, 123: 12, 183: 13, 241: 20, 271: 15}.items():
    DEF[_t]["sight_h"] = _h
SEA = 75   # Two Continents' sea level, main+0x1427F (the engine map)
