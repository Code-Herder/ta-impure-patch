"""airpairs.py -- load air.npz as consecutive (t-1, t) pairs per slot, with named fields.

P = pairs("air.npz") gives dicts P.c (current) and P.p (previous) and P.pp (t-2, where valid:
P.has_pp) of int64 arrays, plus P.typ, P.gt, P.slot.  A pair needs the slot live at t-1 and t
and not created at t (the same unit); t-2 likewise for has_pp.
"""
import numpy as np

M = 0x118
FIELDS = {  # name: (byte offset, size, signed)
    "bank": (0x64, 2, True), "h": (0x66, 2, False), "roll": (0x68, 2, True),
    "x": (0x6A, 4, True), "y": (0x6E, 4, True), "z": (0x72, 4, True),
    "bucket": (0x82, 4, False), "carried": (0x86, 4, False), "type": (0xA6, 2, False),
    "state": (0x110, 4, False), "hp": (0x108, 2, False),
    "vx": (M + 0x08, 4, True), "vy": (M + 0x0C, 4, True), "vz": (M + 0x10, 4, True),
    "ax": (M + 0x14, 4, True), "ay": (M + 0x18, 4, True), "az": (M + 0x1C, 4, True),
    "sp": (M + 0x20, 4, True), "tr": (M + 0x24, 2, True), "m26": (M + 0x26, 2, False),
    "m28": (M + 0x28, 2, False), "stamp": (M + 0x2A, 4, False), "layer": (M + 0x2E, 1, False),
    "m2f": (M + 0x2F, 1, False), "m00": (M + 0x00, 4, False), "m04": (M + 0x04, 4, False),
}


def field(S, off, size, signed):
    b = np.ascontiguousarray(S.view(np.uint8).reshape(len(S), -1)[:, off:off + size])
    dt = {1: "i1", 2: "<i2", 4: "<i4"}[size] if signed else {1: "u1", 2: "<u2", 4: "<u4"}[size]
    return b.view(dt)[:, 0].astype(np.int64)


class Pairs:
    def __init__(self, path):
        z = np.load(path)
        tick, slot, cre, S = z["tick"], z["slot"], z["created"], z["S"]
        o = np.lexsort((tick, slot))
        tick, slot, cre, S = tick[o], slot[o], cre[o], S[o]
        self.gametime = z["gametime"]
        same = (slot[1:] == slot[:-1]) & (tick[1:] == tick[:-1] + 1) & ~cre[1:]
        i1 = np.flatnonzero(same) + 1          # rows that are t, with t-1 at i1-1
        ok2 = np.zeros(len(i1), bool)
        cand = i1 - 2
        good = cand >= 0
        ok2[good] = same[np.maximum(i1[good] - 2, 0)]
        self.has_pp = ok2 & (i1 >= 2)
        self.i = i1
        self.tick = tick[i1]
        self.slot = slot[i1]
        self.gt = self.gametime[self.tick]
        F = {k: field(S, *v) for k, v in FIELDS.items()}
        self.c = {k: v[i1] for k, v in F.items()}
        self.p = {k: v[i1 - 1] for k, v in F.items()}
        self.pp = {k: v[np.maximum(i1 - 2, 0)] for k, v in F.items()}
        self.typ = self.c["type"]
        self.S = S
        self.n = len(i1)


def s32(a):
    a = np.asarray(a, np.int64) & 0xFFFFFFFF
    return np.where(a >= 1 << 31, a - (1 << 32), a)


def asr16_mul(a, b):
    """(a * b) >> 16 as the engine's _allmul + _allshr(16): 64-bit product, arithmetic shift,
    low dword kept."""
    return s32((np.asarray(a, np.int64) * np.asarray(b, np.int64)) >> 16)
