#!/usr/bin/env python3
"""roundtrip.py -- the lossless motion columns (30 Hz, x, z, altitude, heading, each the
difference from the line through the unit's two previous values), written as bytes and read
back: every value of every live tick must come back exactly.

    roundtrip.py <datadir>

A block (300 iterations): varints of [ticks that write anything, samples], then the ticks'
offsets from the previous one, each tick's count, the slot gaps, and the four residual columns.
A life's first value is its create's image (the recording's RK_UNIT, unchanged here); the
decoder is handed the lives' spans and first values in its place. The heading is carried
unwrapped (each step the shortest turn) and compared as the u16 it is.
"""
import sys
import zlib
from pathlib import Path

import numpy as np
import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
import motion as M  # noqa: E402

BLOCK = M.BLOCK


def read_varints(buf, pos, count):
    out = np.empty(count, dtype=np.int64)
    for i in range(count):
        shift = 0
        u = 0
        while True:
            b = buf[pos]
            pos += 1
            u |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        out[i] = (u >> 1) ^ -(u & 1)
    return out, pos


def main():
    d = Path(sys.argv[1])
    V, offs, slot, r0, T = M.load(d)
    if len(sys.argv) > 2:
        # the reduced stream: x, z, altitude truncated to `bits` fraction bits, the heading to
        # 8 bits; the round trip then checks the reduced values come back exactly
        bits = int(sys.argv[2])
        V[:, :3] >>= (16 - bits)
        V[:, 3] = (V[:, 3] + 128) >> 8
    n = len(V)
    seg_of = np.repeat(np.arange(len(offs) - 1), np.diff(offs))
    row = r0[seg_of] + (np.arange(n) - offs[seg_of])
    same = np.zeros(n, dtype=bool)
    same[1:] = seg_of[1:] == seg_of[:-1]
    has2 = np.zeros(n, dtype=bool)
    has2[2:] = same[2:] & same[1:-1]
    # residuals: the line through the two previous values, else the previous value
    res = np.zeros_like(V)
    res[same] = V[same] - V[np.nonzero(same)[0] - 1]
    j = np.nonzero(has2)[0]
    res[j] = V[j] - (2 * V[j - 1] - V[j - 2])
    emit = same & np.any(V != np.roll(V, 1, 0), axis=1)
    ei = np.nonzero(emit)[0]
    order = np.lexsort((slot[seg_of[ei]], row[ei]))
    ei = ei[order]
    et, es, er = row[ei], slot[seg_of[ei]], res[ei]
    blocks = []
    for b0 in range(0, T, BLOCK):
        m = (et >= b0) & (et < b0 + BLOCK)
        tt, ss, rr = et[m], es[m], er[m]
        ut, cnt = np.unique(tt, return_counts=True)
        sg = np.diff(np.concatenate(([0], ss)))
        newt = np.concatenate(([True], tt[1:] != tt[:-1])) if len(tt) else np.zeros(0, bool)
        sg[newt] = ss[newt]
        data = M.varint(np.array([len(ut), len(tt)])) + \
            M.varint(np.diff(np.concatenate(([b0], ut)))) + M.varint(cnt) + M.varint(sg) + \
            b"".join(M.varint(rr[:, c]) for c in range(4))
        blocks.append(data)
    zc = zstandard.ZstdCompressor(level=19)
    packed = [zc.compress(b) for b in blocks]
    defl = 0
    for b in blocks:
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        defl += len(c.compress(b) + c.flush())
    print(f"raw {sum(map(len, blocks)) / 1e6:.2f} MB, zstd19 {sum(map(len, packed)) / 1e6:.2f} MB,"
          f" deflate2k {defl / 1e6:.2f} MB", flush=True)

    # ---- the decoder: only the packed blocks, the lives' spans and their first values
    S = int(slot.max()) + 1
    v1 = np.zeros((S, 4), dtype=np.int64)
    v2 = np.zeros((S, 4), dtype=np.int64)
    nh = np.zeros(S, dtype=np.int64)
    starts = {}
    ends = {}
    for si in range(len(offs) - 1):
        starts.setdefault(int(r0[si]), []).append(si)
        ends.setdefault(int(r0[si] + offs[si + 1] - offs[si] - 1), []).append(si)
    live = np.zeros(S, dtype=bool)
    v_prev2 = np.zeros((S, 4), dtype=np.int64)
    idx = np.full((T, S), -1, dtype=np.int64)
    for si in range(len(offs) - 1):
        a = int(r0[si])
        m = int(offs[si + 1] - offs[si])
        idx[a:a + m, int(slot[si])] = np.arange(offs[si], offs[si + 1])
    zd = zstandard.ZstdDecompressor()
    bad = hd_bad = total = 0
    for bi, pk in enumerate(packed):
        buf = zd.decompress(pk)
        b0 = bi * BLOCK
        (nt, ns), pos = read_varints(buf, 0, 2)
        dt, pos = read_varints(buf, pos, nt)
        cnt, pos = read_varints(buf, pos, nt)
        sg, pos = read_varints(buf, pos, ns)
        cols = []
        for c in range(4):
            r, pos = read_varints(buf, pos, ns)
            cols.append(r)
        rr = np.stack(cols, 1)
        ut = b0 + np.cumsum(dt)
        k = 0
        ti = 0
        for t in range(b0, min(T, b0 + BLOCK)):
            # 1. the units live before t: v1 holds t-1, v2 t-2; held, or decoded where the
            #    tick writes them, from the line through t-2 and t-1
            act = np.nonzero(live)[0]
            if ti < nt and ut[ti] == t:
                c = int(cnt[ti])
                sl = np.cumsum(sg[k:k + c])
                pred = np.where((nh[sl] >= 2)[:, None], 2 * v1[sl] - v2[sl], v1[sl])
                upd = pred + rr[k:k + c]
                k += c
                ti += 1
            else:
                sl = np.zeros(0, dtype=np.int64)
                upd = np.zeros((0, 4), dtype=np.int64)
            v2[act] = v1[act]
            v1[sl] = upd
            nh[act] += 1
            # 2. the creates at t: a life's first value
            for si in starts.get(t, ()):
                s = int(slot[si])
                v1[s] = V[offs[si]]
                v2[s] = 0
                nh[s] = 1
                live[s] = True
            # 3. every live unit's value at t
            act = np.nonzero(live)[0]
            ii = idx[t, act]
            ok = ii >= 0
            bad += int((~ok).sum())
            ref = V[ii[ok]]
            got = v1[act][ok]
            bad += int(np.any(ref[:, :3] != got[:, :3], axis=1).sum())
            hd_bad += int(((ref[:, 3] & 0xFFFF) != (got[:, 3] & 0xFFFF)).sum())
            total += int(ok.sum())
            # 4. the deaths: a life's last tick is t
            for si in ends.get(t, ()):
                live[int(slot[si])] = False
    print(f"decoded {total:,} live unit-ticks of {n:,}; x/z/alt differing {bad}, heading differing "
          f"{hd_bad}")



if __name__ == "__main__":
    main()
