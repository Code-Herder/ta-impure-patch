"""sim.py -- replay extract.py's unit ops tick by tick into whole-world state arrays.

ticks(npz) yields, for each stream iteration in order:
    Tick(gametime, block, S, live, dslots, dmask, created, gone)
S is the (SLOTS, 82) uint32 state AFTER the iteration's records (a view that the next iteration
overwrites: copy what you keep), live a bool (SLOTS,), dslots the delta slots (ascending), dmask
their (n, 82) bool masks, created / gone the slots with an RK_UNIT / RK_UNIT_GONE this iteration.
"""
from collections import namedtuple

import numpy as np

SLOTS = int(__import__("os").environ.get("SIZELAB_SLOTS", "2501"))   # unit slots: 2501 at unit limit 250, 5001 at 500
NDW = 82
OP_TICK, OP_UNIT, OP_DELTA, OP_GONE, OP_CORRECT = 0, 1, 2, 3, 4

Tick = namedtuple("Tick", "gametime block S live dslots dmask dvals created gone")


def load(path):
    z = np.load(path)
    return {k: z[k] for k in z.files}


def ticks(z):
    ops = z["ops"]
    dslot, dmask, dvoff, vals, images = z["dslot"], z["dmask"], z["dvoff"], z["vals"], z["images"]
    S = np.zeros((SLOTS, NDW), np.uint32)
    live = np.zeros(SLOTS, bool)
    tick_rows = np.flatnonzero(ops[:, 0] == OP_TICK)
    ends = list(tick_rows[1:]) + [len(ops)]
    for r0, r1 in zip(tick_rows, ends):
        gt, blk = int(ops[r0, 2]), int(ops[r0, 1])
        body = ops[r0 + 1:r1]
        created, gone = [], []
        dl = body[body[:, 0] == OP_DELTA]
        for op, bi, a, b in body[body[:, 0] != OP_DELTA]:
            if op == OP_UNIT:
                S[a] = images[b]
                live[a] = True
                created.append(int(a))
            elif op == OP_GONE:
                live[a] = False
                S[a] = 0
                gone.append(int(a))
        if len(dl):
            idx = dl[:, 3]
            slots = dslot[idx]
            m = np.unpackbits(dmask[idx], axis=1, bitorder="little")[:, :NDW].astype(bool)
            v0, v1 = dvoff[idx[0]], dvoff[idx[-1] + 1]
            rows = S[slots]
            rows[m] = vals[v0:v1]
            S[slots] = rows
            dv = vals[v0:v1]
        else:
            slots = np.zeros(0, np.int32)
            m = np.zeros((0, NDW), bool)
            dv = np.zeros(0, np.uint32)
        yield Tick(gt, blk, S, live, slots, m, dv, created, gone)


def field(S, off, size, signed=True):
    """The field at byte `off` of every row of S, as int64."""
    b = S.view(np.uint8)[:, off:off + size]
    dt = {1: "i1", 2: "<i2", 4: "<i4"}[size] if signed else {1: "u1", 2: "<u2", 4: "<u4"}[size]
    return np.ascontiguousarray(b).view(dt)[:, 0].astype(np.int64)
