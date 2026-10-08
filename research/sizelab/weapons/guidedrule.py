"""guidedrule.py -- L0 (the rule of guided.py, importable): does a projectile's velocity follow the guided rule (DISASSEMBLED 0x49BA74..0x49BABE)
  vy = S(pitch +0x38, speed +0x3A); h = C(pitch, speed); vx = -S(heading +0x36, h); vz = -C(heading, h)
with S = 0x4B70EF, C = 0x4B7123 (the sine table 0x509F00, imul, +0x1000, shrd 13), evaluated on the
record's own (new) angles and speed? Per weapon: survivor transitions, those whose velocity changed,
rule exact on the new record, and exact among the changed ones."""
import sys, os, collections
import numpy as np
sys.path.insert(1, str(__import__("pathlib").Path(__file__).resolve().parents[1] / "weapons"))
os.environ.setdefault("TA_PRISTINE", "pristine/TotalA.exe.pristine")
sys.path.insert(1, str(__import__("pathlib").Path(__file__).resolve().parents[1] / "units"))
from sintab import T as SIN
from pool import iterations, match
import codec as C
SIN = SIN.astype(np.int64)

def _mul(e, s):
    v = (SIN[e] * s + 0x1000) >> 13          # 64-bit arithmetic shift; shrd keeps the low 32 bits
    return v & 0xFFFFFFFF
def S(a, s):   # a: uint16 angle, s: int32
    a = a.astype(np.int64); a = np.where(a >= 32768, a - 65536, a)
    e = (((a + 0x20) & 0xFFFFFFFF) >> 6 & 0x3FE) >> 1
    return _mul(e, s)
def Cc(a, s):
    e = (((a.astype(np.int64) + 0x4020) & 0xFFFFFFFF) >> 6 & 0x3FE) >> 1
    return _mul(e, s)
def s32(x): x = x.astype(np.int64); return np.where(x >= 2**31, x - 2**32, x)

def rule(R):
    """R: uint32 rows [n, 27] -> predicted (vx, vy, vz) as uint32 [n, 3]"""
    b = R.astype("<u4").view(np.uint8).reshape(len(R), 108)
    head = b[:, 0x36].astype(np.int64) | (b[:, 0x37].astype(np.int64) << 8)
    pitch = b[:, 0x38].astype(np.int64) | (b[:, 0x39].astype(np.int64) << 8)
    sp = s32(b[:, 0x3A:0x3E].copy().view("<u4").ravel())
    vy = S(pitch, sp); h = s32(Cc(pitch, sp))
    vx = (-s32(S(head, h))) & 0xFFFFFFFF; vz = (-s32(Cc(head, h))) & 0xFFFFFFFF
    return np.stack([vx, vy, vz], 1).astype(np.uint32)

