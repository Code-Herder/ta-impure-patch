#!/usr/bin/env python3
"""mp_cells.py -- how much of a mode-3 stream's RK_CELLS is the cells' unit slots and yard bit,
which a mode-3 playing never takes from the recording (tagpu_replay.c: the slots and the yard bit
are the engine's own on such a level; recmp_compare.py checks them instead of comparing).

    mp_cells.py FILE

Per RK_CELLS record of the stream (outside and inside feature brackets separately): the cells
whose change from the grid before it lies only in bytes 0..3 (the two unit slots) or bit 1 of
byte 0x0C (the yard bit), and the stream deflated per block with and without those cells (a
record keeps the cells with any other change, as runs).
"""
import collections
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402

CB = T.REC_CELL_BYTES


def deflate(data):
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def rec(kind, body):
    return struct.pack("<HH", kind, len(body)) + body + bytes(-len(body) % 4)


def main(path):
    sizes = []
    bl, stop = T.blocks(path, sizes)
    h = T.segments(bl)[0]["header"]
    gw, gh, _, _ = T.feature_dims(h)
    grid = bytearray(gw * gh * CB)
    st = collections.Counter()
    today = after = 0
    ticks = 0
    for kind, f, l, payload in bl:
        if kind != T.RB_STREAM:
            continue
        out = bytearray()
        bracket = 0
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                ticks += 1
            first = ticks <= 1
            if rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):
                bracket += 1
            elif rk in (T.RK_EV_FEATURE_END, T.RK_EV_DEATH_END):
                bracket = max(0, bracket - 1)
            if rk != T.RK_CELLS:
                out += rec(rk, b)
                continue
            x, z, n, _ = struct.unpack_from("<HHHH", b)
            where = "inside a bracket" if bracket else "images"
            keep, run = [], None
            for i in range(n):
                at = ((z * gw) + x + i) * CB
                new = b[8 + i * CB:8 + (i + 1) * CB]
                old = grid[at:at + CB]
                other = bytes(new[4:12]) != bytes(old[4:12]) or (new[12] & ~2) != (old[12] & ~2)
                slot = bytes(new[:4]) != bytes(old[:4]) or (new[12] & 2) != (old[12] & 2)
                st[(where, "cells")] += 1
                if first:
                    st[(where, "first iteration")] += 1
                elif other:
                    st[(where, "other bytes changed")] += 1
                elif slot:
                    st[(where, "only slots/yard changed")] += 1
                else:
                    st[(where, "unchanged")] += 1
                grid[at:at + CB] = new
                if first or other or bracket:
                    if run is None:
                        run = [x + i, bytearray()]
                    run[1] += new
                elif run is not None:
                    keep.append(run)
                    run = None
            if run is not None:
                keep.append(run)
            for cx, data in keep:
                out += rec(rk, struct.pack("<HHHH", cx, z, len(data) // CB, 0) + bytes(data))
            if rk == T.RK_CELLS and not bracket:
                pass
        today += deflate(bytes(payload))
        after += deflate(bytes(out))
    print(f"{path}")
    for k, v in sorted(st.items()):
        print(f"  {k[0]:<18}{k[1]:<26}{v:>10}")
    print(f"  stream recompressed {today / 1024:,.1f} KiB; without slot/yard-only cells "
          f"{after / 1024:,.1f} KiB ({(today - after) / 1024:,.1f} KiB less)")


if __name__ == "__main__":
    main(sys.argv[1])
