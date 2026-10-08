"""pool.py -- the projectile pool of a recording, iteration by iteration (shared by the p* scripts).

iterations(path) yields one dict per iteration of the stream:
  tick, count, hw, pool (np.uint32 [hw, 27], a copy), recs (the iteration's projectile records as
  the file holds them: kind, body), shots (RK_EV_SHOT bodies), icpt, block (stream block index),
  snap_before (True when a snapshot block sits between this iteration and the previous one)
"""
import sys, struct
import numpy as np
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T

ND = T.REC_PROJ_DWORDS
CAP = 3000

def iterations(path, want_recs=True):
    bl, stop = T.blocks(path)
    assert stop is None, stop
    pool = np.zeros((CAP, ND), np.uint32)
    count = hw = 0
    cur = None
    bi = -1
    snap = False
    for kind, first, last, payload in bl:
        if kind == T.RB_SNAPSHOT:
            snap = True
            continue
        if kind != T.RB_STREAM:
            continue
        bi += 1
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                if cur is not None:
                    cur["count"], cur["hw"] = count, hw
                    cur["pool"] = pool[:hw].copy()
                    yield cur
                cur = dict(tick=struct.unpack_from("<I", b)[0], recs=[], shots=[], icpt=[],
                           block=bi, snap_before=snap)
                snap = False
                continue
            if cur is None:
                continue
            if rk == T.RK_PROJ_HEAD:
                c, h = struct.unpack_from("<II", b)
                if h < hw:
                    pool[h:hw] = 0
                count, hw = c, h
                if want_recs: cur["recs"].append((rk, bytes(b)))
            elif rk == T.RK_PROJ:
                i = struct.unpack_from("<H", b)[0]
                pool[i] = np.frombuffer(b[4:4 + T.REC_PROJ_IMAGE], np.uint32)
                if want_recs: cur["recs"].append((rk, bytes(b)))
            elif rk == T.RK_PROJ_DELTA:
                i = struct.unpack_from("<H", b)[0]
                bits = b[2:2 + T.REC_PROJ_BITS]
                words = np.frombuffer(b[T.REC_PROJ_DELTA_HEAD:], np.uint32)
                k = 0
                for d in range(ND):
                    if bits[d >> 3] >> (d & 7) & 1:
                        pool[i, d] = words[k]; k += 1
                assert k == len(words)
                if want_recs: cur["recs"].append((rk, bytes(b)))
            elif rk == T.RK_EV_SHOT:
                cur["shots"].append(bytes(b))
            elif rk == T.RK_EV_INTERCEPT:
                cur["icpt"].append(bytes(b))
    if cur is not None:
        cur["count"], cur["hw"] = count, hw
        cur["pool"] = pool[:hw].copy()
        yield cur

def u16_at(row, off):
    """the u16 at byte offset off of a record image row (np.uint32[27])"""
    b = row.tobytes()
    return struct.unpack_from("<H", b, off)[0]


def match(Q, qc, P, pc, window=64, need=12):
    """The identity map of a transition: for each record j < pc of the new pool P, the index of the
    same projectile in the old pool Q (< qc), or -1 for a new one. Generic: no field is named. The
    compaction keeps the survivors' order and appends go at the end, so the search is monotone: the
    first old record at or after the last match, within `window`, sharing at least `need` of the
    27 dwords. Any map is lossless (the residuals carry the rest); a bad one only costs bytes."""
    out = np.full(pc, -1, np.int64)
    k = 0
    for j in range(pc):
        hi = min(qc, k + window)
        if k < hi:
            eq = (Q[k:hi] == P[j]).sum(axis=1)
            hit = np.nonzero(eq >= need)[0]
            if len(hit):
                m = k + int(hit[0])
                out[j] = m
                k = m + 1
    return out
